#include <muduo/net/Channel.h>
#include <muduo/net/EventLoop.h>

#include <boost/bind.hpp>

#include <stdio.h>
#include <sys/timerfd.h>

using namespace muduo;
using namespace muduo::net;

EventLoop* g_loop;
int timerfd;

void timeout(Timestamp receiveTime)
{
    printf("Timeout!\n");
    uint64_t howmany;
    // 应该不会阻塞吧，因为在能读的时候才调用这个函数
    // 并且必须读，不读的话会一直触发 读 的事件
    // muduo库内部的epool使用电平触发 LT
    ::read(timerfd, &howmany, sizeof(howmany)); 
}

int main(void)
{
    EventLoop loop;  // 主线程创建一个EventLoop对象
    g_loop = &loop;
    
    timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    // 新建一个channel,传入的事件循环就是该线程创建的loop, 要监控的就是timerfd
    Channel channel(&loop, timerfd);
    // 回调函数类型是 void(Timestamp) 传递的参数是 poller_->poll() 返回的事件
    channel.setReadCallback(boost::bind(timeout, _1)); 
    //会调用[channel].Update() --> [EventLoop].UpdateChannel() --> [Poller].UpdateChannel()
    channel.enableReading(); 

    // struct timespec {
    //     time_t tv_sec;                /* Seconds */
    //     long   tv_nsec;               /* Nanoseconds */
    // };

    // struct itimerspec {
    //     struct timespec it_interval;  /* Interval for periodic timer */
    //     struct timespec it_value;     /* Initial expiration */
    // };
    
    struct itimerspec howlong;
    bzero(&howlong, sizeof(howlong));
    howlong.it_value.tv_sec = 1; //it_interval=0, 所以是一次性的定时器
    ::timerfd_settime(timerfd, 0, &howlong, NULL);

    loop.loop(); // 后面应该不会执行了，没有结束循环
    ::close(timerfd);
    return 0;
}