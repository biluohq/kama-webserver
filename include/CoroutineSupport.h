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

class ReaderAwaiter
{
public:
    ReaderAwaiter(std::shared_ptr<TcpConnection> conn) : conn_(conn) {}

    bool await_ready() const
    {
        // 如果输入缓冲区已经有数据，就不挂起，直接读
        return conn_->inputBuffer()->readableBytes() > 0;
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        // 协程挂起时，注册恢复回调
        conn_->setCoReadCallback([h]() mutable
                                 { h.resume(); });
        // 确保 Channel 关注读事件 (Epoll)
        // 注意：kama-webserver 的 Channel 可能默认开启读，这里需确认
        // conn_->getChannel()->enableReading();
    }

    Buffer *await_resume()
    {
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