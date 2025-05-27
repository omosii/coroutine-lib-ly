#include "thread_ly.h"

#include <iostream>
#include <sys/syscall.h>
#include <unistd.h>

namespace sylar
{

// 线程信息   
static thread_local Thread* t_thread = nullptr;
static thread_local std::string t_thread_name = "UNKNOW" ;

pid_t Thread::GetThreadId()
{
    return syscall(SYS_gettid);
}

Thread* Thread::GetThis()
{
    return t_thread;
}

// 获取当前线程的名字
const std::string& Thread::GetName()
{
    return t_thread_name;
}

void Thread::SetName(const std::string &name)
{
    if(t_thread)
    {
        t_thread->m_name = name;
    }
    t_thread_name = name;
}

Thread::Thread(std::function<void()> cb, const std::string &name) :
m_cb(cb), m_name(name)
{
    int rt = pthread_create(&m_thread, NULL, &Thread::run, this);
    if(rt)
    {
        std::cerr << "pthread_create thread fail, rt=" << rt << " name=" << name;
        throw std::logic_error("pthread_create error");
    }
    // 等待线程函数完成初始化
    m_semaphore.wait();
}

Thread::~Thread()
{
    if(m_thread)
    {
        pthread_detach(m_thread);
        m_thread = 0;
    }
}

void Thread::join()
{
    if(m_thread)
    {
        int rt = pthread_join(m_thread, nullptr);
        if (rt) 
        {
            std::cerr << "pthread_join failed, rt = " << rt << ", name = " << m_name << std::endl;
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

void* Thread::run(void* arg)
{
    Thread* thread = (Thread*) arg;

    t_thread = thread;
    t_thread_name = thread->m_name;
    thread->m_id = GetThreadId();

    pthread_setname_np(pthread_self(), thread->m_name.substr(0,15).c_str());

    /**  为什么要交换后执行cb而不是直接执行thread->m_cb
     * 1. ??线程安全与资源所有权转移??
            ?? 避免竞态条件??：直接执行 thread->m_cb() 可能与其他线程修改 m_cb 的操作冲突（例如主线程在启动新线程后立即重置 m_cb）。交换操作通过原子性地转移 m_cb 的所有权到局部变量 cb，确保回调函数在新线程中独占执行，消除并发风险。
            ?? 资源释放控制??：交换后，thread->m_cb 变为空，主线程可安全销毁 Thread 对象或重用 m_cb，而无需担心新线程仍在访问它。
        3. ??逻辑清晰性与生命周期管理??
            ?  ?明确执行边界??：交换后，回调函数 cb 完全由新线程控制，与 Thread 对象解耦。这种设计清晰划分了线程初始化阶段（信号量通知）与任务执行阶段（cb()），避免逻辑交叉。
            ??  异常安全??：若 cb() 抛出异常，交换后的局部变量 cb 会在栈展开时自动析构，确保资源释放，而直接执行 m_cb() 可能遗留资源未清理。
     */
    std::function<void()> cb;
    cb.swap(thread->m_cb);

    // 初始化完成
    thread->m_semaphore.signal();

    cb();
    return 0;
}


}