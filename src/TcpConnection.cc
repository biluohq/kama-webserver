#include <functional>
#include <string>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/sendfile.h>
#include <fcntl.h>  // for open
#include <unistd.h> // for close

#include <TcpConnection.h>
#include <Logger.h>
#include <Socket.h>
#include <Channel.h>
#include <EventLoop.h>

static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    LOG_DEBUG << "CheckLoopNotNull start";
    if (loop == nullptr)
    {
        LOG_FATAL << " mainLoop is null!";
    }
    LOG_DEBUG << "CheckLoopNotNull end";
    return loop;
}

TcpConnection::TcpConnection(EventLoop *loop,
                             const std::string &nameArg,
                             int sockfd,
                             const InetAddress &localAddr,
                             const InetAddress &peerAddr)
    : loop_(CheckLoopNotNull(loop)), name_(nameArg), state_(kConnecting), reading_(true), socket_(new Socket(sockfd)), channel_(new Channel(loop, sockfd)), localAddr_(localAddr), peerAddr_(peerAddr)
// , highWaterMark_(64 * 1024 * 1024) // 64M
{
    LOG_DEBUG << "TcpConnection::TcpConnection start";
    // 下面给channel设置相应的回调函数 poller给channel通知感兴趣的事件发生了 channel会回调相应的回调函数
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleError, this));

    LOG_INFO << "TcpConnection::ctor:[" << name_.c_str() << "]at fd=" << sockfd;
    socket_->setKeepAlive(true);
    LOG_DEBUG << "TcpConnection::TcpConnection end";
}

TcpConnection::~TcpConnection()
{
    LOG_DEBUG << "TcpConnection::~TcpConnection start";
    LOG_INFO << "TcpConnection::dtor[" << name_.c_str() << "]at fd=" << channel_->fd() << "state=" << (int)state_;
    LOG_DEBUG << "TcpConnection::~TcpConnection end";
}

// ================= ReadAwaiter 实现 =================

bool TcpConnection::ReadAwaiter::await_ready() const
{
    // 如果缓冲区有数据，或者连接已断开，直接返回
    return conn_->inputBuffer_.readableBytes() > 0 || !conn_->connected();
}

void TcpConnection::ReadAwaiter::await_suspend(std::coroutine_handle<> h)
{
    // 这里的 channel_ 是可见的，因为包含头文件了
    conn_->channel_->setReadCoroutine(h);
    conn_->enableReading();
}

Buffer *TcpConnection::ReadAwaiter::await_resume()
{
    int savedErrno = 0;
    // 这里 channel_->fd() 也是可见的
    ssize_t n = conn_->inputBuffer_.readFd(conn_->channel_->fd(), &savedErrno);

    if (n == 0)
    {
        conn_->handleClose();
    }
    else if (n < 0)
    {
        errno = savedErrno;
        LOG_ERROR << "TcpConnection::readAwaiter error";
        conn_->handleError();
    }

    // 安全起见，清理句柄
    conn_->channel_->clearReadCoroutine();

    return &conn_->inputBuffer_;
}

// ================= DrainAwaiter 实现 =================

bool TcpConnection::DrainAwaiter::await_ready() const
{
    return conn_->outputBuffer_.readableBytes() == 0 || !conn_->connected();
}

void TcpConnection::DrainAwaiter::await_suspend(std::coroutine_handle<> h)
{
    conn_->writeCoroutine_ = h;
    // 必须开启写事件
    conn_->enableWriting();
}

void TcpConnection::DrainAwaiter::await_resume()
{
    // resume 时什么都不用做，直接返回
}

void TcpConnection::send(const std::string &buf)
{
    LOG_DEBUG << "TcpConnection::send [" << name_.c_str()
              << "] - data size: " << buf.size();

    if (state_ == kConnected)
    {
        if (loop_->isInLoopThread()) // 这种是对于单个reactor的情况 用户调用conn->send时 loop_即为当前线程
        {
            sendInLoop(buf.c_str(), buf.size());
        }
        else
        {
            loop_->runInLoop(
                std::bind(&TcpConnection::sendInLoop, this, buf.c_str(), buf.size()));
        }
    }
    LOG_DEBUG << "TcpConnection::send end";
}

/**
 * 发送数据 应用写的快 而内核发送数据慢 需要把待发送数据写入缓冲区，而且设置了水位回调
 **/
void TcpConnection::sendInLoop(const void *data, size_t len)
{
    LOG_DEBUG << "TcpConnection::sendInLoop [" << name_.c_str()
              << "] - data size: " << len;

    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    if (state_ == kDisconnected) // 之前调用过该connection的shutdown 不能再进行发送了
    {
        LOG_ERROR << "disconnected, give up writing";
    }

    // 表示channel_第一次开始写数据或者缓冲区没有待发送数据
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0)
        {
            remaining = len - nwrote;
            // 如果写完了，且有协程在等待 drain，这通常不会发生(因为ready会跳过)，但为了健壮性
            if (remaining == 0 && writeCoroutine_)
            {
                // 一般不需要在这里 resume，因为 await_ready 会处理 buffer 为空的情况
            }
        }
        else // nwrote < 0
        {
            nwrote = 0;
            if (errno != EWOULDBLOCK) // EWOULDBLOCK表示非阻塞情况下没有数据后的正常返回 等同于EAGAIN
            {
                LOG_ERROR << "TcpConnection::sendInLoop";
                if (errno == EPIPE || errno == ECONNRESET) // SIGPIPE RESET
                {
                    faultError = true;
                }
            }
        }
    }
    /**
     * 说明当前这一次write并没有把数据全部发送出去 剩余的数据需要保存到缓冲区当中
     * 然后给channel注册EPOLLOUT事件，Poller发现tcp的发送缓冲区有空间后会通知
     * 相应的sock->channel，调用channel对应注册的writeCallback_回调方法，
     * channel的writeCallback_实际上就是TcpConnection设置的handleWrite回调，
     * 把发送缓冲区outputBuffer_的内容全部发送完成
     **/
    if (!faultError && remaining > 0)
    {
        outputBuffer_.append((char *)data + nwrote, remaining);
        if (!channel_->isWriting())
        {
            channel_->enableWriting();
        }
    }
    LOG_DEBUG << "TcpConnection::sendInLoop end";
}

void TcpConnection::shutdown()
{
    LOG_DEBUG << "TcpConnection::shutdown [" << name_.c_str() << "]";

    if (state_ == kConnected)
    {
        setState(kDisconnecting);
        loop_->runInLoop(
            std::bind(&TcpConnection::shutdownInLoop, this));
    }
    LOG_DEBUG << "TcpConnection::shutdown end";
}

void TcpConnection::shutdownInLoop()
{
    LOG_DEBUG << "TcpConnection::shutdownInLoop [" << name_.c_str() << "]";

    if (!channel_->isWriting()) // 说明当前outputBuffer_的数据全部向外发送完成
    {
        socket_->shutdownWrite();
    }
    LOG_DEBUG << "TcpConnection::shutdownInLoop end";
}

// 连接建立
void TcpConnection::connectEstablished()
{
    LOG_DEBUG << "TcpConnection::connectEstablished [" << name_.c_str() << "]";

    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading(); // 向poller注册channel的EPOLLIN读事件

    // 新连接建立 执行回调
    connectionCallback_(shared_from_this());
    LOG_DEBUG << "TcpConnection::connectEstablished end";
}
// 连接销毁
void TcpConnection::connectDestroyed()
{
    LOG_DEBUG << "TcpConnection::connectDestroyed [" << name_.c_str() << "]";

    if (state_ == kConnected)
    {
        setState(kDisconnected);
        channel_->disableAll(); // 把channel的所有感兴趣的事件从poller中删除掉
        connectionCallback_(shared_from_this());
    }
    channel_->remove(); // 把channel从poller中删除掉
    LOG_DEBUG << "TcpConnection::connectDestroyed end";
}

// // 读是相对服务器而言的 当对端客户端有数据到达 服务器端检测到EPOLLIN 就会触发该fd上的回调 handleRead取读走对端发来的数据
// void TcpConnection::handleRead(Timestamp receiveTime)
// {
//     // [调试] 确认 Epoll 是否真的触发了
//     LOG_DEBUG << "handleRead called! fd=" << channel_->fd();

//     int savedErrno = 0;
//     ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);

//     LOG_DEBUG << "readFd returned n=" << n; // [调试] 读到了多少字节？

//     if (n > 0) // 有数据到达
//     {
//         if (readCoroutine_)
//         {
//             // LOG_DEBUG << "Waking up coroutine"; // [调试] 唤醒协程

//             // LOG_DEBUG << "Executing coReadCallback_";
//             // coReadCallback_();
//             // LOG_DEBUG << "set coReadCallback_ to nullptr";
//             // coReadCallback_ = nullptr; // 执行完后清空

//             // 1. 先把成员变量里的回调取出来放到局部变量
//             auto co = readCoroutine_;

//             // 2. [关键] 在执行之前，先把成员变量置空！
//             // 这样就腾出了位置。如果 cb() 内部（协程）又设置了新回调，
//             // 它会写入 coReadCallback_，而且不会被后续代码覆盖。
//             LOG_DEBUG << "set coReadCallback_ to nullptr";
//             readCoroutine_ = nullptr;

//             // 如果设置了协程的读回调 就执行协程的resume操作
//             LOG_DEBUG << "Executing coReadCallback_";
//             // 3. 执行回调（唤醒协程）
//             co.resume();
//             LOG_DEBUG << "coReadCallback_ executed.";
//         }
//         else{
//             LOG_DEBUG << "No callback set! Data is sitting in buffer."; // [调试] 没有回调设置！数据正停留在缓冲区。
//         }
//         // else{
//         //     // 已建立连接的用户有可读事件发生了 调用用户传入的回调操作onMessage shared_from_this就是获取了TcpConnection的智能指针
//         //     messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
//         // }
//     }
//     else if (n == 0) // 客户端断开
//     {
//         handleClose();
//         // if (coReadCallback_)
//         // {
//         //     // 如果设置了协程的读回调 就执行协程的resume操作
//         //     coReadCallback_();
//         //     coReadCallback_ = nullptr; // 执行完后清空
//         // }
//     }
//     else // 出错了
//     {
//         errno = savedErrno;
//         LOG_ERROR << "TcpConnection::handleRead";
//         handleError();
//     }
// }

void TcpConnection::handleWrite()
{
    LOG_DEBUG << "TcpConnection::handleWrite [" << name_.c_str() << "]";

    if (channel_->isWriting())
    {
        int savedErrno = 0;
        ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
        if (n > 0)
        {
            outputBuffer_.retrieve(n); // 从缓冲区读取reable区域的数据移动readindex下标
            if (outputBuffer_.readableBytes() == 0)
            {
                channel_->disableWriting();
                // === 修改开始 ===
                // [协程核心] 缓冲区已排空，唤醒等待 drain 的协程
                if (writeCoroutine_)
                {
                    auto co = writeCoroutine_;
                    writeCoroutine_ = nullptr;
                    // LOG_DEBUG << "Resuming write coroutine";
                    co.resume();
                }
                // // 2. 否则执行旧的回调逻辑 (保持兼容性)
                // else if (writeCompleteCallback_)
                // {
                //     loop_->queueInLoop(
                //         std::bind(writeCompleteCallback_, shared_from_this()));
                // }
                // === 修改结束 ===
                if (state_ == kDisconnecting)
                {
                    shutdownInLoop(); // 在当前所属的loop中把TcpConnection删除掉
                }
            }
        }
        else
        {
            LOG_ERROR << "TcpConnection::handleWrite";
        }
    }
    else
    {
        LOG_ERROR << "TcpConnection fd=" << channel_->fd() << "is down, no more writing";
    }
    LOG_DEBUG << "TcpConnection::handleWrite end";
}
void TcpConnection::handleClose()
{
    LOG_INFO << "TcpConnection::handleClose fd=" << channel_->fd() << "state=" << (int)state_;
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr guardThis(shared_from_this());

    // [新增] 清理 Channel 中的读协程
    // Channel 本身没有像 TcpConnection 那样直接持有 handle，
    // 但是我们需要确保如果协程还在 Channel 里挂起，它能被唤醒以退出循环

    // 如果 ReadAwaiter 正在挂起，Channel::readCoroutine_ 应该是有值的
    // 但 Channel 没有公开获取 coroutine 的接口，
    // 不过 handleClose 被调用通常意味着：
    // 1. readFd 返回 0 (在 await_resume 内部调用了 handleClose) -> 协程正在运行，没挂起。
    // 2. 主动 shutdown -> 协程可能挂起。

    // 我们需要一种机制来唤醒挂起的读协程（如果是被动关闭）
    // 由于我们修改了 Channel，如果 HUP 发生，Channel 会自动 resume readCoroutine。
    // 所以这里只要确保 Channel 里的句柄被清空即可。
    channel_->clearReadCoroutine();

    // 唤醒写协程 (保持不变)
    if (writeCoroutine_)
    {
        LOG_INFO << "Resuming write coroutine on close";
        auto co = writeCoroutine_;
        writeCoroutine_ = nullptr;
        co.resume();
    }

    if (connectionCallback_)
    {
        connectionCallback_(guardThis);
    }
    if (closeCallback_)
    {
        closeCallback_(guardThis);
    }
    LOG_DEBUG << "TcpConnection::handleClose end";
}
void TcpConnection::handleError()
{
    LOG_DEBUG << "TcpConnection::handleError start";
    int optval;
    socklen_t optlen = sizeof optval;
    int err = 0;
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
    {
        err = errno;
    }
    else
    {
        err = optval;
    }
    LOG_ERROR << "TcpConnection::handleError name:" << name_.c_str() << "- SO_ERROR:%" << err;
    LOG_DEBUG << "TcpConnection::handleError end";
}

// 辅助接口实现
void TcpConnection::enableReading()
{
    LOG_DEBUG << "TcpConnection::enableReading start";
    if (!channel_->isReading())
        channel_->enableReading();
    LOG_DEBUG << "TcpConnection::enableReading end";
}
void TcpConnection::enableWriting()
{
    LOG_DEBUG << "TcpConnection::enableWriting start";
    if (!channel_->isWriting())
        channel_->enableWriting();
    LOG_DEBUG << "TcpConnection::enableWriting end";
}

// // 新增的零拷贝发送函数
// void TcpConnection::sendFile(int fileDescriptor, off_t offset, size_t count)
// {
//     if (connected())
//     {
//         if (loop_->isInLoopThread())
//         { // 判断当前线程是否是loop循环的线程
//             sendFileInLoop(fileDescriptor, offset, count);
//         }
//         else
//         { // 如果不是，则唤醒运行这个TcpConnection的线程执行Loop循环
//             loop_->runInLoop(
//                 std::bind(&TcpConnection::sendFileInLoop, shared_from_this(), fileDescriptor, offset, count));
//         }
//     }
//     else
//     {
//         LOG_ERROR << "TcpConnection::sendFile - not connected";
//     }
// }

// // 在事件循环中执行sendfile
// void TcpConnection::sendFileInLoop(int fileDescriptor, off_t offset, size_t count)
// {
//     ssize_t bytesSent = 0;    // 发送了多少字节数
//     size_t remaining = count; // 还要多少数据要发送
//     bool faultError = false;  // 错误的标志位

//     if (state_ == kDisconnecting)
//     { // 表示此时连接已经断开就不需要发送数据了
//         LOG_ERROR << "disconnected, give up writing";
//         return;
//     }

//     // 表示Channel第一次开始写数据或者outputBuffer缓冲区中没有数据
//     if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
//     {
//         bytesSent = sendfile(socket_->fd(), fileDescriptor, &offset, remaining);
//         if (bytesSent >= 0)
//         {
//             remaining -= bytesSent;
//             if (remaining == 0 && writeCompleteCallback_)
//             {
//                 // remaining为0意味着数据正好全部发送完，就不需要给其设置写事件的监听。
//                 loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
//             }
//         }
//         else
//         { // bytesSent < 0
//             if (errno != EWOULDBLOCK)
//             { // 如果是非阻塞没有数据返回错误这个是正常显现等同于EAGAIN，否则就异常情况
//                 LOG_ERROR << "TcpConnection::sendFileInLoop";
//             }
//             if (errno == EPIPE || errno == ECONNRESET)
//             {
//                 faultError = true;
//             }
//         }
//     }
//     // 处理剩余数据
//     if (!faultError && remaining > 0)
//     {
//         // 继续发送剩余数据
//         loop_->queueInLoop(
//             std::bind(&TcpConnection::sendFileInLoop, shared_from_this(), fileDescriptor, offset, remaining));
//     }
// }