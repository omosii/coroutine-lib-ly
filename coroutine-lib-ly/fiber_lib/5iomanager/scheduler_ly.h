#ifndef _SCHEDULER_LY_H_
#define _SCHEDULER_LY_H_

#include "thread_ly.h"
#include "fiber_ly.h"

#include <mutex>
#include <vector>

namespace sylar{

class Scheduler
{
public:
    // 线程数量 是否将主线程作为工作线程 调度器名称
    Scheduler(size_t threads = 1, bool use_caller = true, const std::string& name = "Scheduler");
    virtual ~Scheduler();

    const std::string& getName() const {return m_name;}

public:
    // 获取正在运行的调度器
    static Scheduler* GetThis();

protected:
    // 设置正在运行的调度器
    void SetThis();

public:
    // 添加任务到任务队列 FiberOrCb是调度任务类型，可以是协程或者函数
    template <class FiberOrCb>
    void scheduleLock(FiberOrCb fc, int thread = -1)
    {   
        bool need_tickle;// 用于标记任务队列是否为空，从而判断是否要唤醒线程
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // empty ->  all thread is idle -> need to be waken up
            need_tickle = m_tasks.empty();

            ScheduleTask task(fc, thread);// 创建任务对象
            if(task.fiber || task.cb)       
            {
                m_tasks.push_back(task);
            }     
        }

        if(need_tickle) // 若队列为空唤醒线程 
        {
            tickle();
        }
    }

	// 启动线程池 
    virtual void start();
    // 关闭线程池
    virtual void stop();

protected:
    // 唤醒线程
    virtual void tickle();

    // 线程函数
    virtual void run();

    // 空闲协程函数， 无任务时，执行idle协程
    virtual void idle();

    // 是否可以关闭
    virtual bool stopping();

    // 返回是否有空闲线程
    // 具体来说，当调度协程进入idle时，空闲线程数+1；从idle协程返回时，空闲线程数-1
    bool hasIdleThread() {return m_idleThreadCount > 0;}

private:
    // 任务
    struct ScheduleTask  
    {
        std::shared_ptr<Fiber> fiber;
        std::function<void()> cb;
        int thread; // 指定任务需要运行的线程id

        ScheduleTask()
        {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }

        ScheduleTask(std::shared_ptr<Fiber> f, int thr)
        {
            fiber = f;
            thread = thr;
        }

        ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
        {
            // f 是一个指向 std::shared_ptr<Fiber> 的指针，即 shared_ptr 的指针。
            fiber.swap(*f); // 通过 swap 转移所有权
            thread = thr;
        }

        ScheduleTask(std::function<void()> f, int thr)
        {
            cb = f;
            thread = thr;
        }

        ScheduleTask(std::function<void()> *f, int thr)
        {
            cb.swap(*f);
            thread = thr;
        }

        void reset()
        {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }
    };
private:
    std::string m_name;
    // 互斥锁 -> 保护任务队列
	std::mutex m_mutex;
    // 线程池
    std::vector<std::shared_ptr<Thread>> m_threads;
    // 任务队列
    std::vector<ScheduleTask> m_tasks;
    // 存储工作线程的线程id
    std::vector<int> m_threadIds;
    // 需要额外创建的线程数
    size_t m_threadCount = 0;
    // 活跃线程数
    std::atomic<size_t> m_activeThreadCount = {0}; // 赋值号= 可省略
    // 空闲线程数
    std::atomic<size_t> m_idleThreadCount = {0};

    // 主线程是否用作工作线程
    bool m_useCaller;
    // 如果是 -> 需要额外创建调度协程
    std::shared_ptr<Fiber> m_schedulerFiber;
    // 如果是 -> 记录主线程的线程id
    int m_rootThread = -1;
    // 是否正在关闭
    bool m_stopping = false;
};


}


#endif