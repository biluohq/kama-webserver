#ifndef TIMER_H
#define TIMER_H

#include "noncopyable.h"
#include "Timestamp.h"
#include <atomic> // 必须包含这个
#include <functional>

/**
 * Timer用于描述一个定时器
 * 定时器回调函数，下一次超时时刻，重复定时器的时间间隔等
 */
class Timer : noncopyable
{
public:
    using TimerCallback = std::function<void()>;

    Timer(TimerCallback cb, Timestamp when, double interval)
        : callback_(move(cb)),
          expiration_(when),
          interval_(interval),
          repeat_(interval > 0.0), // 一次性定时器设置为0
          sequence_(s_numCreated_++)
    {
    }

    void run() const 
    { 
        callback_(); 
    }

    Timestamp expiration() const  { return expiration_; }
    bool repeat() const { return repeat_; }

    // [修复] 必须要有这个 sequence() 接口
    int64_t sequence() const { return sequence_; }

    // 重启定时器(如果是非重复事件则到期时间置为0)
    void restart(Timestamp now);

    static int64_t numCreated() { return s_numCreated_; }

private:
    const TimerCallback callback_;  // 定时器回调函数
    Timestamp expiration_;          // 下一次的超时时刻
    const double interval_;         // 超时时间间隔，如果是一次性定时器，该值为0
    const bool repeat_;             // 是否重复(false 表示是一次性定时器)

    // [修复] 必须要有 sequence_ 成员
    const int64_t sequence_;

    static std::atomic_int64_t s_numCreated_;
};

#endif // TIMER_H