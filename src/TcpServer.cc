#include <functional>
#include <string.h>

#include <TcpServer.h>
#include <Logger.h>
#include <TcpConnection.h>

static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    LOG_DEBUG << "CheckLoopNotNull start";
    if (loop == nullptr)
    {
        LOG_FATAL << "main Loop is NULL!";
    }
    LOG_DEBUG << "CheckLoopNotNull end";
    return loop;
}

TcpServer::TcpServer(EventLoop *loop,
                     const InetAddress &listenAddr,
                     const std::string &nameArg,
                     Option option)
    : loop_(CheckLoopNotNull(loop)), ipPort_(listenAddr.toIpPort()), name_(nameArg), acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)), threadPool_(new EventLoopThreadPool(loop, name_)), connectionCallback_(), messageCallback_(), nextConnId_(1), started_(0)
{
    LOG_DEBUG << "TcpServer::TcpServer start";
    // // 当有新用户连接时，Acceptor类中绑定的acceptChannel_会有读事件发生，执行handleRead()调用TcpServer::newConnection回调
    // acceptor_->setNewConnectionCallback(
    //     std::bind(&TcpServer::newConnection, this, std::placeholders::_1, std::placeholders::_2));
    LOG_DEBUG << "TcpServer::TcpServer end";
}

TcpServer::~TcpServer()
{
    LOG_DEBUG << "TcpServer::~TcpServer start";
    for (auto &item : connections_)
    {
        TcpConnectionPtr conn(item.second);
        item.second.reset(); // 把原始的智能指针复位 让栈空间的TcpConnectionPtr conn指向该对象 当conn出了其作用域 即可释放智能指针指向的对象
        // 销毁连接
        conn->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed, conn));
    }
    LOG_DEBUG << "TcpServer::~TcpServer end";
}

// 设置底层subloop的个数
void TcpServer::setThreadNum(int numThreads)
{
    LOG_DEBUG << "TcpServer::setThreadNum [" << name_.c_str() << "] threads " << numThreads;

    int numThreads_ = numThreads;
    threadPool_->setThreadNum(numThreads_);
    LOG_DEBUG << "TcpServer::setThreadNum end";
}

// 开启服务器监听
void TcpServer::start()
{
    LOG_DEBUG << "TcpServer::start [" << name_.c_str() << "] starting";

    if (started_.fetch_add(1) == 0) // 防止一个TcpServer对象被start多次
    {
        threadPool_->start(threadInitCallback_); // 启动底层的loop线程池
        loop_->runInLoop([this]()
                         {
            acceptor_->listen();
            // [新增] 启动 Accept 协程
            acceptLoop(); });
        LOG_DEBUG << "TcpServer::start end";
    }
}

// [新增] Accept 协程：永不停止的循环
Task TcpServer::acceptLoop()
{
    LOG_DEBUG << "TcpServer::acceptLoop start";
    LOG_INFO << "AcceptLoop coroutine started";

    // 只要服务器在运行
    while (true)
    {
        // 1. [挂起] 等待新连接
        // 这一步会把协程句柄注册到 Acceptor 的 Channel 里
        auto [connfd, peerAddr, err] = co_await acceptor_->accept();

        // 2. [恢复] 收到新连接
        if (connfd >= 0)
        {
            if (started_ > 0) // 简单的运行状态检查
            {
                handleNewConnection(connfd, peerAddr);
            }
            else
            {
                ::close(connfd);
            }
        }
        else
        {
            LOG_ERROR << "accept error: " << err;
            // 可以在这里处理 EMFILE (文件描述符耗尽) 等情况
            if (err == EMFILE)
            {
                ::sleep(1); // 简单的缓流策略
            }
        }
    }

    LOG_DEBUG << "AcceptLoop coroutine ended";
}

// 有一个新用户连接，acceptor会执行这个回调操作，负责将mainLoop接收到的请求连接(acceptChannel_会有读事件发生)通过回调轮询分发给subLoop去处理
void TcpServer::handleNewConnection(int sockfd, const InetAddress &peerAddr)
{
    LOG_DEBUG << "TcpServer::handleNewConnection " << peerAddr.toIpPort().c_str();

    // 轮询算法 选择一个subLoop 来管理connfd对应的channel
    EventLoop *ioLoop = threadPool_->getNextLoop();
    char buf[64] = {0};
    snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
    ++nextConnId_; // 这里没有设置为原子类是因为其只在mainloop中执行 不涉及线程安全问题
    std::string connName = name_ + buf;

    LOG_INFO << "TcpServer::newConnection [" << name_.c_str() << "]- new connection [" << connName.c_str() << "]from " << peerAddr.toIpPort().c_str();

    // 通过sockfd获取其绑定的本机的ip地址和端口信息
    sockaddr_in local;
    ::memset(&local, 0, sizeof(local));
    socklen_t addrlen = sizeof(local);
    if (::getsockname(sockfd, (sockaddr *)&local, &addrlen) < 0)
    {
        LOG_ERROR << "sockets::getLocalAddr";
    }

    InetAddress localAddr(local);
    TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                            connName,
                                            sockfd,
                                            localAddr,
                                            peerAddr));
    connections_[connName] = conn;
    conn->setConnectionCallback(connectionCallback_);

    // 设置了如何关闭连接的回调
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));

    ioLoop->runInLoop(
        std::bind(&TcpConnection::connectEstablished, conn));

    LOG_DEBUG << "TcpServer::handleNewConnection done";
}

void TcpServer::removeConnection(const TcpConnectionPtr &conn)
{
    LOG_DEBUG << "TcpServer::removeConnection [" << name_.c_str() << "] - connection %s" << conn->name().c_str();

    loop_->runInLoop(
        std::bind(&TcpServer::removeConnectionInLoop, this, conn));
    LOG_DEBUG << "TcpServer::removeConnection end";
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn)
{
    LOG_DEBUG << "TcpServer::removeConnectionInLoop start";
    LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_.c_str() << "] - connection %s" << conn->name().c_str();

    connections_.erase(conn->name());
    EventLoop *ioLoop = conn->getLoop();
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn));

    LOG_DEBUG << "TcpServer::removeConnectionInLoop end";
}