#include <coroutine>
#include <iostream>
#include <thread>
#include <chrono>

// 辅助函数：获取当前线程ID并转为字符串
auto get_tid()
{
    return std::this_thread::get_id();
}

struct MyAwaitable
{
    bool await_ready() const noexcept { return false; }

    // 在这里发生“控制权交接”
    void await_suspend(std::coroutine_handle<> h) const noexcept
    {
        std::cout << "[Thread " << get_tid() << "] 协程挂起 (Suspended)\n";

        // 关键点：我们在 Thread A 里，开启了一个新的 Thread B
        // 并把句柄 h 传给了 Thread B
        std::thread([h]() mutable
                    {
            std::cout << "[Thread " << get_tid() << "] 新线程启动，准备恢复协程...\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // 在 Thread B 里调用 resume()
            h.resume(); })
            .detach();
    }

    void await_resume() const noexcept
    {
        // 这里的代码将在 resume() 的那个线程里执行
    }
};

struct Task
{
    struct promise_type
    {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

Task foo()
{
    std::cout << "[Thread " << get_tid() << "] 协程开始 (Start)\n";

    // 1. 在主线程挂起
    co_await MyAwaitable{};

    // 2. 在新线程恢复
    // 注意：这里的代码虽然在 foo 函数里，但已经是在另一个线程跑了！
    std::cout << "[Thread " << get_tid() << "] 协程恢复 (Resumed) -> 发生线程迁移了！\n";
}

int main()
{
    std::cout << "[Thread " << get_tid() << "] 主函数开始\n";

    foo();

    // 强行阻塞主线程，等待子线程演示完毕
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << "[Thread " << get_tid() << "] 主函数结束\n";

    return 0;
}