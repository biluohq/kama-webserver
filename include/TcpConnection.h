#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <coroutine> // [新增]

#include "noncopyable.h"
#include "InetAddress.h"
#include "Callbacks.h"
#include "Buffer.h"
#include "Timestamp.h"
#include "Logger.h"
#include "TimerId.h"

class Channel;
class EventLoop;
class Socket;

/**
 * TcpServer => Acceptor => 有一个新用户连接，通过accept函数拿到connfd
 * => TcpConnection设置回调 => 设置到Channel => Poller => Channel回调
 **/
/**
 * TcpConnection (Coroutine Refactored Version)
 * * 核心变化：
 * 1. 内置 read() 和 drain() 的 Awaitable 支持。
 * 2. 内部维护 readCoroutine_ 和 writeCoroutine_ 句柄。
 */
class TcpConnection : noncopyable, public std::enable_shared_from_this<TcpConnection>
{
public:
    TcpConnection(EventLoop *loop,
                  const std::string &nameArg,
                  int sockfd,
                  const InetAddress &localAddr,
                  const InetAddress &peerAddr);
    ~TcpConnection();

    EventLoop *getLoop() const { return loop_; }
    const std::string &name() const { return name_; }
    const InetAddress &localAddress() const { return localAddr_; }
    const InetAddress &peerAddress() const { return peerAddr_; }

    bool connected() const { return state_ == kConnected; }
    bool disconnected() const { return state_ == kDisconnected; }

    Buffer *inputBuffer() { return &inputBuffer_; }
    // [新增] 1. 暴露 outputBuffer 给 Awaiter 检查状态
    Buffer *outputBuffer() { return &outputBuffer_; }
    Channel *channel() { return channel_.get(); }

    // 发送数据
    void send(const std::string &buf);
    
    // 关闭半连接
    void shutdown();

    // ================== 协程核心接口 ==================

    // [Reader Awaiter]
    // 用法: Buffer* buf = co_await conn->read();
    // [修改] ReadAwaiter 只保留声明
    struct ReadAwaiter
    {
        TcpConnection *conn_;
        ReadAwaiter(TcpConnection *conn) : conn_(conn) {}

        bool await_ready() const;
        void await_suspend(std::coroutine_handle<> h);
        Buffer *await_resume();
    };

    // [修改] DrainAwaiter 只保留声明
    struct DrainAwaiter
    {
        TcpConnection *conn_;
        DrainAwaiter(TcpConnection *conn) : conn_(conn) {}

        bool await_ready() const;
        void await_suspend(std::coroutine_handle<> h);
        void await_resume();
    };

    // 获取读等待器
    ReadAwaiter read() { return ReadAwaiter(this); }
    // 获取写排空等待器
    DrainAwaiter drain() { return DrainAwaiter(this); }

    // [SendFile Awaiter]
    // 用法: ssize_t bytesSent = co_await conn->sendFile(fd, offset, count);
    // 零拷贝发送文件，协程化接口
    struct SendFileAwaiter
    {
        TcpConnection *conn_;
        int fileFd_;
        off_t offset_;
        size_t count_;

        SendFileAwaiter(TcpConnection *conn, int fileFd, off_t offset, size_t count)
            : conn_(conn), fileFd_(fileFd), offset_(offset), count_(count) {}

        bool await_ready() const;
        void await_suspend(std::coroutine_handle<> h);
        ssize_t await_resume();
    };

    // 获取 sendFile 等待器
    SendFileAwaiter sendFile(int fileFd, off_t offset, size_t count)
    {
        return SendFileAwaiter(this, fileFd, offset, count);
    }

    // [ReadWithTimeout Awaiter]
    // 用法: auto [buf, timedOut] = co_await conn->readWithTimeout(30.0);
    // 带超时的异步读取，防止客户端恶意挂起
    struct ReadResult
    {
        Buffer *buffer;
        bool timedOut;
    };

    struct ReadWithTimeoutAwaiter
    {
        // 共享状态，用于协调 read 事件和 timer 回调
        struct State
        {
            std::coroutine_handle<> handle;
            std::atomic<bool> resumed{false};
            bool timedOut = false;
            TimerId timerId;
        };

        TcpConnection *conn_;
        double timeoutSecs_;
        std::shared_ptr<State> state_;

        ReadWithTimeoutAwaiter(TcpConnection *conn, double timeoutSecs)
            : conn_(conn), timeoutSecs_(timeoutSecs), state_(std::make_shared<State>()) {}

        bool await_ready() const;
        void await_suspend(std::coroutine_handle<> h);
        ReadResult await_resume();
    };

    ReadWithTimeoutAwaiter readWithTimeout(double timeoutSecs)
    {
        return ReadWithTimeoutAwaiter(this, timeoutSecs);
    }

    // [WriteAwaiter]
    // 用法: size_t written = co_await conn->write(data);
    // 或: size_t written = co_await conn->write(data, 1024*1024); // 自定义高水位
    // 带背压控制的写入，当输出缓冲区超过高水位时自动挂起
    static constexpr size_t kDefaultHighWaterMark = 64 * 1024 * 1024;

    struct WriteAwaiter
    {
        TcpConnection *conn_;
        std::string data_;
        size_t highWaterMark_;

        WriteAwaiter(TcpConnection *conn, std::string data, size_t highWaterMark)
            : conn_(conn), data_(std::move(data)), highWaterMark_(highWaterMark) {}

        bool await_ready() const;
        void await_suspend(std::coroutine_handle<> h);
        size_t await_resume();
    };

    WriteAwaiter write(const std::string &data, size_t highWaterMark = kDefaultHighWaterMark)
    {
        return WriteAwaiter(this, data, highWaterMark);
    }

    WriteAwaiter write(std::string &&data, size_t highWaterMark = kDefaultHighWaterMark)
    {
        return WriteAwaiter(this, std::move(data), highWaterMark);
    }

    // ================================================

    void setConnectionCallback(const ConnectionCallback &cb)
    { connectionCallback_ = cb; }
    void setCloseCallback(const CloseCallback &cb)
    { closeCallback_ = cb; }

    // 供 Server 调用
    // 连接建立
    void connectEstablished();
    // 连接销毁
    void connectDestroyed();

    // 给 Awaiter 用的内部接口
    void enableReading(); 
    void enableWriting();

private:
    enum StateE
    {
        kDisconnected, // 已经断开连接
        kConnecting,   // 正在连接
        kConnected,    // 已连接
        kDisconnecting // 正在断开连接
    };
    void setState(StateE state) { state_ = state; }

    // void handleRead(Timestamp receiveTime);
    void handleWrite();//处理写事件
    void handleClose();
    void handleError();

    void sendInLoop(const void *data, size_t len);
    void shutdownInLoop();
    EventLoop *loop_; // 这里是baseloop还是subloop由TcpServer中创建的线程数决定 若为多Reactor 该loop_指向subloop 若为单Reactor 该loop_指向baseloop
    const std::string name_;
    std::atomic_int state_;
    bool reading_;//连接是否在监听读事件

    // Socket Channel 这里和Acceptor类似    Acceptor => mainloop    TcpConnection => subloop
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;

    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    // 这些回调TcpServer也有 用户通过写入TcpServer注册 TcpServer再将注册的回调传递给TcpConnection TcpConnection再将回调注册到Channel中
    ConnectionCallback connectionCallback_;       // 有新连接时的回调
    CloseCallback closeCallback_; // 关闭连接的回调


    // 协程句柄 (取代了 std::function 回调)
    std::coroutine_handle<> writeCoroutine_ = nullptr;
    size_t writeResumeThreshold_ = 0;

    int sendFileFd_ = -1;
    off_t sendFileOffset_ = 0;
    size_t sendFileRemaining_ = 0;
    ssize_t sendFileBytesSent_ = 0;

    // 数据缓冲区
    Buffer inputBuffer_;    // 接收数据的缓冲区
    Buffer outputBuffer_;   // 发送数据的缓冲区 用户send向outputBuffer_发
};
