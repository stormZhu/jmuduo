// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#define __STDC_LIMIT_MACROS
#include <muduo/net/TimerQueue.h>

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Timer.h>
#include <muduo/net/TimerId.h>

#include <boost/bind.hpp>

#include <sys/timerfd.h>

namespace muduo
{
namespace net
{
namespace detail
{

int createTimerfd()
{
  int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                 TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerfd < 0)
  {
    LOG_SYSFATAL << "Failed in timerfd_create";
  }
  return timerfd;
}

// 计算超时时刻与当前时刻的时间差
struct timespec howMuchTimeFromNow(Timestamp when)
{
  // 超时时刻的us - 当前时刻的 us
  int64_t microseconds = when.microSecondsSinceEpoch()
                         - Timestamp::now().microSecondsSinceEpoch();
  if (microseconds < 100)
  {
    microseconds = 100;  //微秒？
  }
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(
      microseconds / Timestamp::kMicroSecondsPerSecond); // Timestamp::kMicroSecondsPerSecond = 1000*1000
  ts.tv_nsec = static_cast<long>(
      (microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
  return ts;
}

void readTimerfd(int timerfd, Timestamp now)
{
  uint64_t howmany;
  ssize_t n = ::read(timerfd, &howmany, sizeof howmany);
  LOG_TRACE << "TimerQueue::handleRead() " << howmany << " at " << now.toString();
  if (n != sizeof howmany)
  {
    LOG_ERROR << "TimerQueue::handleRead() reads " << n << " bytes instead of 8";
  }
}

void resetTimerfd(int timerfd, Timestamp expiration)
{
  // wake up loop by timerfd_settime()
  struct itimerspec newValue;
  struct itimerspec oldValue;
  bzero(&newValue, sizeof newValue);
  bzero(&oldValue, sizeof oldValue);
  newValue.it_value = howMuchTimeFromNow(expiration);
  int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
  if (ret)
  {
    LOG_SYSERR << "timerfd_settime()";
  }
}

}
}
}

using namespace muduo;
using namespace muduo::net;
using namespace muduo::net::detail;

TimerQueue::TimerQueue(EventLoop* loop)
  : loop_(loop),
    timerfd_(createTimerfd()),
    timerfdChannel_(loop, timerfd_),
    timers_(),
    callingExpiredTimers_(false)
{
  timerfdChannel_.setReadCallback(
      boost::bind(&TimerQueue::handleRead, this));
  // we are always reading the timerfd, we disarm it with timerfd_settime.
  timerfdChannel_.enableReading();
}

TimerQueue::~TimerQueue()
{
  ::close(timerfd_);
  // do not remove channel, since we're in EventLoop::dtor();
  for (TimerList::iterator it = timers_.begin();
      it != timers_.end(); ++it)
  {
    delete it->second;
  }
}

TimerId TimerQueue::addTimer(const TimerCallback& cb,
                             Timestamp when,
                             double interval)
{
  Timer* timer = new Timer(cb, when, interval);
  // loop_->runInLoop(
  //     boost::bind(&TimerQueue::addTimerInLoop, this, timer));
  addTimerInLoop(timer);
  return TimerId(timer, timer->sequence());
}

void TimerQueue::cancel(TimerId timerId)
{
  // loop_->runInLoop(
  //     boost::bind(&TimerQueue::cancelInLoop, this, timerId));
  cancelInLoop(timerId);
}

void TimerQueue::addTimerInLoop(Timer* timer)
{
  loop_->assertInLoopThread();
  // 插入一个定时器， 有可能会使得最早到期的定时器发生改变
  bool earliestChanged = insert(timer);

  if (earliestChanged) //如果最早到期的定时器发生了改变
  {
    // 重置定时器的超时时刻 (timerfd_settime)
    resetTimerfd(timerfd_, timer->expiration());
  }
}

void TimerQueue::cancelInLoop(TimerId timerId)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());
  ActiveTimer timer(timerId.timer_, timerId.sequence_);
  // 查找要取消的定时器
  ActiveTimerSet::iterator it = activeTimers_.find(timer);
  if (it != activeTimers_.end()) // 如果找到了， 就从activeTimers_和timers_中移除
  {
    size_t n = timers_.erase(Entry(it->first->expiration(), it->first));
    assert(n == 1); (void)n;
    delete it->first; // FIXME: no delete please
    activeTimers_.erase(it);
  }
  else if (callingExpiredTimers_) // 正在调用回调函数
  {
    // 已经到期，并且正在调用回调函数的定时器，没法删除，所以先放入队列中
    cancelingTimers_.insert(timer); //cancelingTimers_中的定时器会在reset的时候清理掉
  }
  assert(timers_.size() == activeTimers_.size());
}

void TimerQueue::handleRead()
{
  loop_->assertInLoopThread();
  Timestamp now(Timestamp::now());
  readTimerfd(timerfd_, now);

  // 返回当前时间(超时才会触发这个函数)之前所有的定时器列表
  std::vector<Entry> expired = getExpired(now);

  callingExpiredTimers_ = true;
  cancelingTimers_.clear();
  // safe to callback outside critical section
  for (std::vector<Entry>::iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    //这里回调定时器处理函数
    it->second->run();
  }
  callingExpiredTimers_ = false;

  // 如果不是一次性定时器，需要重启
  reset(expired, now);
}

std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
  assert(timers_.size() == activeTimers_.size());
  std::vector<Entry> expired; // 先申明一个空的
  Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX));
  // 返回第一个 未到期 的Timer的迭代器
  // lower_bound的含义是返回第一个值>=sentry的元素的iterator
  // 即*end >= sentry, 从而end->first > now (不是>=) 不可能存在和sentry相等的
  TimerList::iterator end = timers_.lower_bound(sentry);
  assert(end == timers_.end() || now < end->first);
  // 将到期的定时器插入到expired
  std::copy(timers_.begin(), end, back_inserter(expired)); // 就是将到期的定时器按顺序放到expired中
  // 从timers_中移除到期的定时器
  timers_.erase(timers_.begin(), end);

  // 从activeTimers_中移除到期的定时器
  // 感觉挺慢的，还有构造timer，还要erase
  for (std::vector<Entry>::iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    ActiveTimer timer(it->second, it->second->sequence());
    size_t n = activeTimers_.erase(timer);
    assert(n == 1); (void)n;
  }

  assert(timers_.size() == activeTimers_.size());
  return expired; // 返回一个局部vector, 会有返回值优化RVO
}

void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now)
{
  Timestamp nextExpire;
  // expired 已经超时的定时器
  for (std::vector<Entry>::const_iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    ActiveTimer timer(it->second, it->second->sequence());
    // 如果是重复的定时器并且是未取消定时器， 则重启该定时器
    if (it->second->repeat()
        && cancelingTimers_.find(timer) == cancelingTimers_.end())
    {
      it->second->restart(now);
      insert(it->second); //再次插入到定时器集合中
    }
    else // 是一次性定时器 或者 是要取消的定时器
    {
      // FIXME move to a free list
      delete it->second; // FIXME: no delete please
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
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size()); // 存的是同样的定时器列表
  bool earliestChanged = false;
  Timestamp when = timer->expiration(); //得到新添加进来的定时器的到期时间
  TimerList::iterator it = timers_.begin();
  // 如果timers_为空或者when小于timers_中的最早到期时间 timers_ (set)默认是从小到大排序的
  if (it == timers_.end() || when < it->first)
  {
    earliestChanged = true;
  }
  {
    std::pair<TimerList::iterator, bool> result
      = timers_.insert(Entry(when, timer));
    assert(result.second); (void)result;
  }
  {
    std::pair<ActiveTimerSet::iterator, bool> result
      = activeTimers_.insert(ActiveTimer(timer, timer->sequence()));
    assert(result.second); (void)result;
  }

  assert(timers_.size() == activeTimers_.size());
  return earliestChanged;
}

