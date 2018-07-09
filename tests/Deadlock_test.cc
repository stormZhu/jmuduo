// 一个在多线程程序里fork造成死锁的例子
// 一个输出示例：
/*
pid = 19445 Entering main ...
pid = 19445 begin doit ...
pid = 19447 begin doit ...
pid = 19445 end doit ...
pid = 19445 Exiting main ...

父进程创建了一个线程，并对mutex加锁，
父进程创建一个子进程，在子进程中调用doit，由于子进程会复制父进程的内存，这时候mutex处于锁的状态，
父进程在复制子进程的时候，只会复制当前线程的执行状态，其他线程不会复制，因此子进程会处于死锁的状态。
如果也复制了父进程的子线程，就不会死锁（问题就是它不会复制!）
父进程结束的时候，子进程会怎么样？？ 
孤儿进程: 父进程先于子进程结束，则子进程成为孤儿进程，
则子进程成为孤儿进程ºinit进程，称为init进程领养孤儿进程。
*/

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void* doit(void* arg)
{
    printf("pid = %d begin doit ...\n", static_cast<int>(getpid()));
    pthread_mutex_lock(&mutex);
    struct timespec ts = {2,0};
    nanosleep(&ts, NULL);
    pthread_mutex_unlock(&mutex);
    printf("pid = %d end doit ...\n", static_cast<int>(getpid()));
    return NULL;
}

int main(void)
{
    printf("pid = %d Entering main ...\n", static_cast<int>(getpid()));
    pthread_t tid;
    pthread_create(&tid, NULL, doit, NULL);
    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    //执行到这里的时候，子线程已经执行了doit，获得了锁，然后在等待2s
    //1s之后，主进程fork出子进程 子è¿程也开始调用doit
    //子进程复制主进程相同的资源（不会复制主进程的子线程！），互斥量mutex也复制了，而且处于加锁状态！
    //子进程就进入doit时，已经处于lock状态，还请求锁，就发生了死锁!
    if (fork() == 0)
    {
        doit(NULL);
    }
    pthread_join(tid, NULL);
    printf("pid = %d Exiting main ...\n", static_cast<int>(getpid()));
    return 0;
}
