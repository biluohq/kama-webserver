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
    // void sendFile(int fileDescriptor, off_t offset, size_t count); 
    
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
    // void sendFileInLoop(int fileDescriptor, off_t offset, size_t count);
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

    // 数据缓冲区
    Buffer inputBuffer_;    // 接收数据的缓冲区
    Buffer outputBuffer_;   // 发送数据的缓冲区 用户send向outputBuffer_发
};
