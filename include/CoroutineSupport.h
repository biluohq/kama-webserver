// include/CoroutineSupport.h
#pragma once
#include <coroutine>
#include <exception>
#include <memory>

#include "TcpConnection.h"
#include "EventLoop.h"


// 协程的返回类型，通常命名为 Task 或 CoReturn
struct Task
{
    struct promise_type
    {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

struct SleepAwaiter
{
    EventLoop *loop_;
    double delay_;

    SleepAwaiter(EventLoop *loop, double delay) : loop_(loop), delay_(delay) {}

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h)
    {
        // 协程挂起时，注册恢复回调
        LOG_DEBUG << "await_suspend called. Registering callback.";
        conn_->setCoReadCallback([h]() mutable
                                 {
            LOG_DEBUG << "Coroutine resumed by callback!";
            h.resume(); });
        // 确保 Channel 关注读事件 (Epoll)
        // 注意：kama-webserver 的 Channel 可能默认开启读，这里需确认
        // conn_->getChannel()->enableReading();
    }

    Buffer *await_resume()
    {
        LOG_DEBUG << "await_resume called.";
        // 协程恢复（或 await_ready 返回 true）时调用
        // 返回缓冲区指针供业务逻辑读取
        return conn_->inputBuffer();
    }

private:
    std::shared_ptr<TcpConnection> conn_;
};

// 辅助函数，添加到 TcpConnection 或作为全局函数
inline ReaderAwaiter asyncRead(std::shared_ptr<TcpConnection> conn)
{
    return ReaderAwaiter(conn);
}

class DrainAwaiter
{
public:
    DrainAwaiter(std::shared_ptr<TcpConnection> conn) : conn_(conn) {}

    // await_ready: 如果缓冲区已经空了，就不需要挂起，直接返回
    bool await_ready() const
    {
        return conn_->outputBuffer()->readableBytes() == 0;
    }

    // await_suspend: 缓冲区不为空，挂起协程，并注册唤醒回调
    void await_suspend(std::coroutine_handle<> h)
    {
        conn_->setCoWriteCompleteCallback([h]() mutable
                                          { h.resume(); });

        // 确保 Channel 正在监听写事件，否则 handleWrite 永远不会触发
        if (!conn_->channel()->isWriting())
        {
            conn_->channel()->enableWriting();
        }
    }

    // await_resume: 恢复时无返回值
    void await_resume() {}

private:
    std::shared_ptr<TcpConnection> conn_;
};

// 辅助函数：语义更清晰
inline DrainAwaiter asyncDrain(std::shared_ptr<TcpConnection> conn)
{
    return DrainAwaiter(conn);
}

// === 基于 EventLoop/TimerQueue 的协程友好定时工具 ===

class SleepAwaiter
{
public:
    SleepAwaiter(EventLoop *loop, double seconds, std::shared_ptr<TcpConnection> conn = nullptr)
        : loop_(loop), seconds_(seconds), conn_(std::move(conn)) {}

    // 延迟时间<=0 时无需挂起
    bool await_ready() const
    {
        return seconds_ <= 0.0;
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        // 使用弱引用防止连接/协程已经结束时继续 resume
        std::weak_ptr<TcpConnection> weakConn = conn_;
        loop_->runAfter(seconds_, [h, weakConn]() mutable {
            if (auto conn = weakConn.lock())
            {
                if (!conn->connected())
                {
                    return;
                }
            }
            // 如果没有绑定具体连接（直接用 EventLoop），或者连接仍然有效，则恢复协程
            h.resume();
        });
    }

    void await_resume() {}

private:
    EventLoop *loop_;
    double seconds_;
    std::shared_ptr<TcpConnection> conn_;
};

// 以 EventLoop 为粒度的睡眠
inline SleepAwaiter asyncSleep(EventLoop *loop, double seconds)
{
    return SleepAwaiter(loop, seconds);
}

// 以 TcpConnection 为粒度的睡眠，自动绑定到连接所在的 EventLoop
inline SleepAwaiter asyncSleep(const std::shared_ptr<TcpConnection> &conn, double seconds)
{
    return SleepAwaiter(conn->getLoop(), seconds, conn);
}
