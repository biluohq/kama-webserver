#include <EventLoop.h>
#include <Channel.h>
#include <Logger.h>
#include <Timer.h>
#include <TimerQueue.h>

#include <sys/timerfd.h>
#include <unistd.h>
#include <string.h>

int createTimerfd()
{
    /**
     * CLOCK_MONOTONIC：绝对时间
     * TFD_NONBLOCK：非阻塞
     */
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                    TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0)
    {
        LOG_ERROR << "Failed in timerfd_create";
    }
    return timerfd;
}

struct timespec howMuchTimeFromNow(Timestamp when)
{
    int64_t microseconds = when.microSecondsSinceEpoch() - Timestamp::now().microSecondsSinceEpoch();
    if (microseconds < 100)
    {
        microseconds = 100;
    }
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(microseconds / Timestamp::kMicroSecondsPerSecond);
    ts.tv_nsec = static_cast<long>((microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
    return ts;
}

void resetTimerfd(int timerfd, Timestamp expiration)
{
    struct itimerspec newValue;
    struct itimerspec oldValue;
    memset(&newValue, 0, sizeof newValue);
    memset(&oldValue, 0, sizeof oldValue);
    newValue.it_value = howMuchTimeFromNow(expiration);
    ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
}

void readTimerfd(int timerfd, Timestamp now)
{
    uint64_t howmany;
    ssize_t n = ::read(timerfd, &howmany, sizeof howmany);
    if (n != sizeof howmany)
    {
        LOG_ERROR << "TimerQueue::handleRead() reads " << n << " bytes instead of 8";
    }
}

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timerfd_(createTimerfd()),
      timerfdChannel_(loop_, timerfd_),
      timers_()
{
    timerfdChannel_.setReadCallback(
        std::bind(&TimerQueue::handleRead, this));
    timerfdChannel_.enableReading();
}

TimerQueue::~TimerQueue()
{   
    timerfdChannel_.disableAll();
    timerfdChannel_.remove();
    ::close(timerfd_);
    // 删除所有定时器
    for (const Entry& timer : timers_)
    {
        delete timer.second;
    }
}

TimerId TimerQueue::addTimer(TimerCallback cb,
                             Timestamp when,
                             double interval)
{
    Timer* timer = new Timer(std::move(cb), when, interval);
    loop_->runInLoop(
        std::bind(&TimerQueue::addTimerInLoop, this, timer));
    // 返回 TimerId (包含指针和序列号)
    return TimerId(timer, timer->sequence());
}

// [新增] 取消接口实现
void TimerQueue::cancel(TimerId timerId)
{
    loop_->runInLoop(std::bind(&TimerQueue::cancelInLoop, this, timerId));
}

void TimerQueue::addTimerInLoop(Timer* timer)
{
    // 是否取代了最早的定时触发时间
    bool eraliestChanged = insert(timer);

    // 我们需要重新设置timerfd_触发时间
    if (eraliestChanged)
    {
        resetTimerfd(timerfd_, timer->expiration());
    }
}

void TimerQueue::cancelInLoop(TimerId timerId)
{
    ActiveTimer timer(timerId.timer_, timerId.sequence_); // 使用 friend class 访问私有成员

    // 在 activeTimers_ 中查找
    auto it = activeTimers_.find(timer);
    if (it != activeTimers_.end())
    {
        // 如果找到了，从两个集合中都移除
        size_t n = timers_.erase(Entry(it->first->expiration(), it->first));
        delete it->first; // FIXME: 使用 unique_ptr 会更好
        activeTimers_.erase(it);
    }
    else if (callingExpiredTimers_)
    {
        // 如果正在执行回调过程中（即定时器已经过期被移出 timers_），
        // 则加入取消列表，防止它如果设置了 repeat 又被重新加入
        cancelingTimers_.insert(timer);
    }
}

// 重置timerfd
void TimerQueue::resetTimerfd(int timerfd_, Timestamp expiration)
{
    struct itimerspec newValue;
    struct itimerspec oldValue;
    memset(&newValue, '\0', sizeof(newValue));
    memset(&oldValue, '\0', sizeof(oldValue));

    // 超时时间 - 现在时间
    int64_t microSecondDif = expiration.microSecondsSinceEpoch() - Timestamp::now().microSecondsSinceEpoch();
    if (microSecondDif < 100)
    {
        microSecondDif = 100;
    }

    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(
        microSecondDif / Timestamp::kMicroSecondsPerSecond);
    ts.tv_nsec = static_cast<long>(
        (microSecondDif % Timestamp::kMicroSecondsPerSecond) * 1000);
    newValue.it_value = ts;
    // 此函数会唤醒事件循环
    if (::timerfd_settime(timerfd_, 0, &newValue, &oldValue))
    {
        LOG_ERROR << "timerfd_settime faield()";
    }
}

void ReadTimerFd(int timerfd) 
{
    uint64_t read_byte;
    ssize_t readn = ::read(timerfd, &read_byte, sizeof(read_byte));
    
    if (readn != sizeof(read_byte)) {
        LOG_ERROR << "TimerQueue::ReadTimerFd read_size < 0";
    }
}

// 返回删除的定时器节点 （std::vector<Entry> expired）
std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
    std::vector<Entry> expired;
    Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX));
    TimerList::iterator end = timers_.lower_bound(sentry);
    std::copy(timers_.begin(), end, back_inserter(expired));
    timers_.erase(timers_.begin(), end);

    // [新增] 同步移除 activeTimers_
    for (const auto &entry : expired)
    {
        ActiveTimer timer(entry.second, entry.second->sequence());
        size_t n = activeTimers_.erase(timer);
    }

    return expired;
}

void TimerQueue::handleRead()
{
    Timestamp now = Timestamp::now();
    ReadTimerFd(timerfd_);

    std::vector<Entry> expired = getExpired(now);

    // 遍历到期的定时器，调用回调函数
    callingExpiredTimers_ = true;
    cancelingTimers_.clear();
    for (const Entry& it : expired)
    {
        it.second->run();
    }
    callingExpiredTimers_ = false;
    
    // 重新设置这些定时器
    reset(expired, now);

}

void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now)
{
    Timestamp nextExpire;
    for (const Entry& it : expired)
    {
        ActiveTimer timer(it.second, it.second->sequence());
        // 重复任务则继续执行
        if (it.second->repeat() && cancelingTimers_.find(timer) == cancelingTimers_.end())
        {
            auto timer = it.second;
            timer->restart(Timestamp::now());
            insert(timer);
        }
        else
        {
            delete it.second;
        }
    }

    if (!timers_.empty())
    {
        nextExpire = timers_.begin()->second->expiration();
    }

    if (nextExpire.valid())
    {
        resetTimerfd(timerfd_, nextExpire);
    }
}

bool TimerQueue::insert(Timer* timer)
{
    bool earliestChanged = false;
    Timestamp when = timer->expiration();
    TimerList::iterator it = timers_.begin();
    if (it == timers_.end() || when < it->first)
    {
        // 说明最早的定时器已经被替换了
        earliestChanged = true;
    }

    // 定时器管理红黑树插入此新定时器
    // [新增] 同时插入 timers_ 和 activeTimers_
    timers_.insert(Entry(when, timer));
    activeTimers_.insert(ActiveTimer(timer, timer->sequence()));

    return earliestChanged;
}