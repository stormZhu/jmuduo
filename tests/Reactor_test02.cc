#include <muduo/net/EventLoop.h>

#include <stdio.h>

using namespace muduo;
using namespace muduo::net;

EventLoop* g_loop;

void threadFunc()
{
    g_loop->loop(); // 子线程调用主线程创建的EventLoop对象
}

int main(void)
{
    EventLoop loop;  // 主线程创建一个EventLoop对象
    g_loop = &loop;
    Thread t(threadFunc);
    t.start();
    t.join();
    return 0;
}