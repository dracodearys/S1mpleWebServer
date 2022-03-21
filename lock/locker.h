#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// POSIX信号量
class sem
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)    // pshared为0表示只在单个进程中使用。信号量初始值为0。
        {
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)  // 信号量初始值为num。
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);    // 丢弃信号量
    }
    // 信号量值-1，相当于加锁。
    bool wait()
    {
        return sem_wait(&m_sem) == 0;   
    }
    // 信号量值+1，相当于解锁。
    bool post()
    {
        return sem_post(&m_sem) == 0;   
    }

private:
    sem_t m_sem;     // 声明未命名信号量，只在单个进程中使用。
};

// 互斥量mutex
class locker
{
public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};
// 条件变量
class cond
{
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;   // 唤醒一个等待该条件的线程
    }
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;    // 唤醒等待该条件的所有线程
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};
#endif
