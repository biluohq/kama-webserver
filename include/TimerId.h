#pragma once

#include <cstdint>

// 前向声明，不需要包含 Timer.h
class Timer;

/**
 * TimerId
 * 一个不透明的句柄，用于取消定时器。
 * 它包含一个 Timer 指针和一个序列号(sequence)，
 * 序列号用于防止地址复用导致的误取消。
 */
class TimerId
{
public:
    TimerId()
        : timer_(nullptr), sequence_(0)
    {
    }

    TimerId(Timer *timer, int64_t seq)
        : timer_(timer), sequence_(seq)
    {
    }

    // 允许 TimerQueue 访问私有成员以进行比较和查找
    friend class TimerQueue;

private:
    Timer *timer_;
    int64_t sequence_;
};