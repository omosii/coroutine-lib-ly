#ifndef _THREAD_LY_H_
#define _THREAD_LY_H_

#include <mutex>
#include <condition_variable>
#include <functional>

namespace sylar
{

// 用于线程方法间的同步
class Semaphore
{
private:
    std::mutex mtx;
    std::condition_variable cv;
    int count;
public:
    // 信号量初始化为0
    explicit Semaphore(int count_ = 0) : count(count_){}

    // p
    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx);
        while(count == 0) // 必须用while,避免虚假唤醒与多线程竞争
        {
            cv.wait(lock);
        }
        count--;
    }

    // v
    void signal()
    {
        std::unique_lock<std::mutex> lock(mtx);
        count++;
        cv.notify_one();
    }

};


}

#endif