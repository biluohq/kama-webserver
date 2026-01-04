#include <Timer.h>

// [修复] 初始化静态成员
std::atomic_int64_t Timer::s_numCreated_{0};

void Timer::restart(Timestamp now)
{
    if (repeat_)
    {
        // 如果是重复定时事件，则继续添加定时事件，得到新事件到期事件
        expiration_ = addTime(now, interval_);
    }
    else 
    {
        expiration_ = Timestamp();
    }
}