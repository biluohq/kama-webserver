#pragma once

#include <functional>
#include <coroutine>

#include "noncopyable.h"
#include "Socket.h"
#include "Channel.h"
#include "InetAddress.h"

class EventLoop;
class InetAddress;

class Acceptor : noncopyable
{
public:
    // using NewConnectionCallback = std::function<void(int sockfd, const InetAddress &)>;

    Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport);
    ~Acceptor();
    //设置新连接的回调函数
    // void setNewConnectionCallback(const NewConnectionCallback &cb) { NewConnectionCallback_ = cb; }
    // 判断是否在监听
    bool listenning() const { return listenning_; }
    // 监听本地端口
    void listen();

    // ================= 协程接口 =================

    // 定义 accept 返回的结果
    struct AcceptResult
    {
        int connfd;
        InetAddress peerAddr;
        int err; // 0 表示成功
    };

    struct AcceptAwaiter
    {
        Acceptor *acceptor_;
        AcceptResult result_;

        AcceptAwaiter(Acceptor *acc) : acceptor_(acc) {}

        // Accept 是阻塞操作，永远挂起等待事件
        bool await_ready() const { return false; }

        void await_suspend(std::coroutine_handle<> h)
        {
            // 将协程句柄注册给 Channel
            acceptor_->acceptChannel_->setReadCoroutine(h);
            acceptor_->acceptChannel_->enableReading();
        }

        AcceptResult await_resume()
        {
            // 协程醒来，说明 Channel 触发了 EPOLLIN
            InetAddress peerAddr;
            int connfd = acceptor_->acceptSocket_.accept(&peerAddr);

            if (connfd >= 0)
            {
                return {connfd, peerAddr, 0};
            }
            else
            {
                return {-1, peerAddr, errno};
            }
        }
    };

    // 供 TcpServer 调用: auto [fd, addr, err] = co_await acceptor.accept();
    AcceptAwaiter accept() { return AcceptAwaiter(this); }

private:
    // void handleRead();//处理新用户的连接事件

    EventLoop *loop_; // Acceptor用的就是用户定义的那个baseLoop 也称作mainLoop
    Socket acceptSocket_;//专门用于接收新连接的socket
    Channel *acceptChannel_;// 专门用于监听新连接的channel
    // NewConnectionCallback NewConnectionCallback_;//新连接的回调函数
    bool listenning_;//是否在监听
};