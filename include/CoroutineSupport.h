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
        // [调试日志] 检查是否直接读取
        bool ready = conn_->inputBuffer()->readableBytes() > 0;
        if (ready)
        {
            LOG_DEBUG << "await_ready TRUE. Data already in buffer, skip suspend.";
        }
        else
        {
            LOG_DEBUG << "await_ready FALSE. Buffer empty, preparing to suspend.";
        }
        // 如果输入缓冲区已经有数据，就不挂起，直接读
        return ready;
    }

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