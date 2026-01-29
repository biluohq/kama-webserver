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
