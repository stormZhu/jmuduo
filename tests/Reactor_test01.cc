#include <muduo/net/EventLoop.h>

#include <stdio.h>

using namespace muduo;
using namespace muduo::net;

void threadFunc()
{
    printf("threadFunc(): pid = %d, tid = %d\n",
        getpid(), CurrentThread::tid());
    
    EventLoop loop;  // 子线程也创建一个 EventLoop
    loop.loop();
}

int main(void)
{
    printf("main(): pid = %d, tid = %d\n",
        getpid(), CurrentThread::tid());

    EventLoop loop;  // 主线程创建一个EventLoop对象

    Thread t(threadFunc);
    t.start();

    loop.loop();
    t.join();
    return 0;
}