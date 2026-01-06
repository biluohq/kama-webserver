#include <sys/epoll.h>

#include <Channel.h>
#include <EventLoop.h>
#include <Logger.h>

const int Channel::kNoneEvent = 0; //空事件
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI; //读事件
const int Channel::kWriteEvent = EPOLLOUT; //写事件

// EventLoop: ChannelList Poller
Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop)
    , fd_(fd)
    , events_(0)
    , revents_(0)
    , index_(-1)
    , tied_(false)
{
}

Channel::~Channel()
{
}

// channel的tie方法什么时候调用过?  TcpConnection => channel
/**
 * TcpConnection中注册了Channel对应的回调函数，传入的回调函数均为TcpConnection
 * 对象的成员方法，因此可以说明一点就是：Channel的结束一定晚于TcpConnection对象！
 * 此处用tie去解决TcpConnection和Channel的生命周期时长问题，从而保证了Channel对象能够在
 * TcpConnection销毁前销毁。
 **/
void Channel::tie(const std::shared_ptr<void> &obj)
{
    LOG_DEBUG << "Channel::tie start";
    tie_ = obj;
    tied_ = true;
    LOG_DEBUG << "Channel::tie end";
}
//update 和remove => EpollPoller 更新channel在poller中的状态
/**
 * 当改变channel所表示的fd的events事件后，update负责再poller里面更改fd相应的事件epoll_ctl
 **/
void Channel::update()
{
    LOG_DEBUG << "Channel::update start";
    // 通过channel所属的eventloop，调用poller的相应方法，注册fd的events事件
    loop_->updateChannel(this);
    LOG_DEBUG << "Channel::update end";
}

// 在channel所属的EventLoop中把当前的channel删除掉
void Channel::remove()
{
    LOG_DEBUG << "Channel::remove start";
    loop_->removeChannel(this);
    LOG_DEBUG << "Channel::remove end";
}

void Channel::handleEvent(Timestamp receiveTime)
{
    LOG_DEBUG << "Channel::handleEvent start";
    if (tied_)
    {
        std::shared_ptr<void> guard = tie_.lock();
        if (guard)
        {
            handleEventWithGuard(receiveTime);
        }
        // 如果提升失败了 就不做任何处理 说明Channel的TcpConnection对象已经不存在了
    }
    else
    {
        handleEventWithGuard(receiveTime);
    }
    LOG_DEBUG << "Channel::handleEvent end";
}

void Channel::handleEventWithGuard(Timestamp receiveTime)
{
    LOG_INFO<<"channel handleEvent revents:"<<revents_;
    // 关闭
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) // 当TcpConnection对应Channel 通过shutdown 关闭写端 epoll触发EPOLLHUP
    {
        if (logHup_)
        {
            LOG_WARN << "fd = " << fd_ << " Channel::handle_event() EPOLLHUP";
        }

        // 如果有协程在等读，HUP 也应该唤醒它（让它读到 0 字节从而感知关闭）
        if (readCoroutine_)
        {
            readCoroutine_.resume();
        }
        else if (closeCallback_)
        {
            closeCallback_();
        }
    }
    // 错误
    if (revents_ & EPOLLERR)
    {
        if (errorCallback_)
        {
            errorCallback_();
        }
    }
    // 读
    if (revents_ & (EPOLLIN | EPOLLPRI))
    {
        // [协程优先]：直接唤醒，绕过 TcpConnection::handleRead
        if (readCoroutine_)
        {
            readCoroutine_.resume();
        }
        else if (readCallback_)
        {
            readCallback_(receiveTime);
        }
    }
    // 写
    if (revents_ & EPOLLOUT)
    {
        if (writeCallback_)
        {
            writeCallback_();
        }
    }

    LOG_DEBUG << "Channel::handleEventWithGuard end";
}