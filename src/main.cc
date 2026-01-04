#include <string>
#include <coroutine>
#include <TcpServer.h>
#include <Logger.h>
#include <sys/stat.h>
#include <sstream>
#include "AsyncLogging.h"
#include "LFU.h"
#include "memoryPool.h"
#include "CoroutineSupport.h"

/**
 * [新增] 协程业务处理函数
 * 替代了原来的 onMessage 回调
 */
Task sessionHandler(std::shared_ptr<TcpConnection> conn)
{
    // 保持连接引用，防止协程挂起期间连接析构
    LOG_INFO << "Coroutine session started for " << conn->name();

    try
    {
        while (conn->connected())
        {
            LOG_INFO << "Waiting for data..."; // [1] 挂起前
            // [新用法] 直接调用成员函数 co_await conn->read()
            Buffer *buf = co_await conn->read();

            // 如果连接断开且没数据，buf 可能为空或 readableBytes 为 0
            if (buf->readableBytes() == 0)
            {
                if (conn->disconnected())
                    break;
                // 可能是虚假唤醒，继续等待
                continue;
            }

            std::string msg = buf->retrieveAllAsString();
            LOG_INFO << "Received: " << msg; // [2] 唤醒后
            
            if (msg.empty())
                continue;

            // 2. [业务逻辑] 判断是否请求大数据测试
            // 如果收到 "load"，则发送 100MB 数据进行压力测试
            if (msg.size() >= 4 && msg.substr(0, 4) == "load")
            {
                LOG_INFO << "Start sending 100MB big data...";

                // 构造 1MB 的数据块
                std::string chunk(1024 * 1024, 'X');
                int totalChunks = 1; // 总共发 1MB

                for (int i = 0; i < totalChunks; ++i)
                {
                    if (!conn->connected())
                        break;

                    conn->send(chunk); // 非阻塞发送，写入应用层 Buffer

                    // 3. [写] 大数据流控核心逻辑
                    // 检查输出缓冲区：如果积压超过 10MB (高水位)，挂起协程等待排空
                    if (conn->outputBuffer()->readableBytes() > 10 * 1024 * 1024)
                    {
                        // LOG_DEBUG << "Buffer high (" << conn->outputBuffer()->readableBytes()
                        //           << "), waiting for drain...";

                        // 挂起！直到内核把数据发完，触发 WriteComplete 回调唤醒此协程
                        co_await conn->drain();
                    }
                }
                LOG_INFO << "Finished sending big data.";
            }
            else
            {
                // 3. [普通逻辑] 简单的 Echo 回显
                conn->send(msg);
            }
        }
    }
    catch (...)
    {
        LOG_ERROR << "Coroutine exception or connection closed";
    }
}

// 日志文件滚动大小为1MB (1*1024*1024字节)
static const off_t kRollSize = 1 * 1024 * 1024;
class EchoServer
{
public:
    EchoServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
        : server_(loop, addr, name), loop_(loop)
    {
        LOG_DEBUG << "EchoServer started";
        // 注册回调函数
        LOG_DEBUG << "Setting connection callback";
        server_.setConnectionCallback(
            std::bind(&EchoServer::onConnection, this, std::placeholders::_1));

        // 设置合适的subloop线程数量
        server_.setThreadNum(3);
    }
    void start()
    {
        LOG_DEBUG << "Starting EchoServer";
        server_.start();
    }

private:
    // 连接建立或断开的回调函数
    void onConnection(const TcpConnectionPtr &conn)
    {
        if (conn->connected())
        {
            LOG_INFO << "Connection UP :" << conn->peerAddress().toIpPort().c_str();
            // [修改] 连接建立成功，启动协程接管该连接
            sessionHandler(conn);
        }
        else
        {
            LOG_INFO << "Connection DOWN :" << conn->peerAddress().toIpPort().c_str();
        }
    }

    // 可读写事件回调
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time)
    {
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg);
        // conn->shutdown();   // 关闭写端 底层响应EPOLLHUP => 执行closeCallback_
    }
    TcpServer server_;
    EventLoop *loop_;
};
AsyncLogging *g_asyncLog = NULL;
AsyncLogging *getAsyncLog()
{
    return g_asyncLog;
}
void asyncLog(const char *msg, int len)
{
    AsyncLogging *logging = getAsyncLog();
    if (logging)
    {
        logging->append(msg, len);
    }
}
int main(int argc, char *argv[])
{
    // 第一步启动日志，双缓冲异步写入磁盘.
    // 创建一个文件夹
    const std::string LogDir = "logs";
    mkdir(LogDir.c_str(), 0755);
    // 使用std::stringstream 构建日志文件夹
    std::ostringstream LogfilePath;
    LogfilePath << LogDir << "/" << ::basename(argv[0]); // 完整的日志文件路径
    AsyncLogging log(LogfilePath.str(), kRollSize);
    g_asyncLog = &log;
    Logger::setOutput(asyncLog); // 为Logger设置输出回调, 重新配接输出位置
    log.start();                 // 开启日志后端线程
    // 第二步启动内存池和LFU缓存
    //  初始化内存池
    memoryPool::HashBucket::initMemoryPool();

    // 初始化缓存
    const int CAPACITY = 5;
    KamaCache::KLfuCache<int, std::string> lfu(CAPACITY);
    // 第三步启动底层网络模块
    EventLoop loop;
    InetAddress addr(8080);
    EchoServer server(&loop, addr, "EchoServer");
    server.start();
    // 主loop开始事件循环  epoll_wait阻塞 等待就绪事件(主loop只注册了监听套接字的fd，所以只会处理新连接事件)
    std::cout << "================================================Start Web Server================================================" << std::endl;
    loop.loop();
    std::cout << "================================================Stop Web Server=================================================" << std::endl;
    // 结束日志打印
    log.stop();
}