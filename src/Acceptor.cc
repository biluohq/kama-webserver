#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include <Acceptor.h>
#include <Logger.h>
#include <InetAddress.h>

static int createNonblocking()
{
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0)
    {
         LOG_FATAL << "listen socket create err " << errno;
    }
    return sockfd;
}

Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
    : loop_(loop)
    , acceptSocket_(createNonblocking())
    , acceptChannel_(new Channel(loop, acceptSocket_.fd()))
    , listenning_(false)
{
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(true);
    acceptSocket_.bindAddress(listenAddr);
    // TcpServer::start() => Acceptor.listen() 如果有新用户连接 要执行一个回调(accept => connfd => 打包成Channel => 唤醒subloop)
    // baseloop监听到有事件发生 => acceptChannel_(listenfd) => 执行该回调函数
    // acceptChannel_.setReadCallback(
    //     std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor()
{
    acceptChannel_->disableAll();    // 把从Poller中感兴趣的事件删除掉
    acceptChannel_->remove();        // 调用EventLoop->removeChannel => Poller->removeChannel 把Poller的ChannelMap对应的部分删除
    delete acceptChannel_;
}

void Acceptor::listen()
{
    LOG_DEBUG << "Acceptor::listen()";
    listenning_ = true;
    acceptSocket_.listen();         // listen
    // acceptChannel_.enableReading(); // acceptChannel_注册至Poller !重要
}