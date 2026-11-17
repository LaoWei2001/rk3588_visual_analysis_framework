#pragma once
#include <pthread.h>

// 供仍使用 pthread 同步原语的线程使用；异常离开作用域时也必须解锁。
class PthreadLock
{
  public:
    explicit PthreadLock(pthread_mutex_t &mutex) : mutex_(mutex)
    {
        pthread_mutex_lock(&mutex_);
    }
    ~PthreadLock()
    {
        pthread_mutex_unlock(&mutex_);
    }
    PthreadLock(const PthreadLock &) = delete;
    PthreadLock &operator=(const PthreadLock &) = delete;

  private:
    pthread_mutex_t &mutex_;
};
