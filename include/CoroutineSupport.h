// include/CoroutineSupport.h
#pragma once
#include <coroutine>
#include <exception>
#include <memory>

#include "TcpConnection.h"


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
        // 利用现有的 runAfter，时间到了执行 resume
        loop_->runAfter(delay_, [h]() mutable
                        { h.resume(); });
    }

    void await_resume() {}
};

// 辅助函数
inline SleepAwaiter sleep(EventLoop *loop, double delay)
{
    return SleepAwaiter(loop, delay);
}