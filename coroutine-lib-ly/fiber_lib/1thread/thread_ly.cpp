#include "thread_ly.h"

#include <iostream>
#include <sys/syscall.h>
#include <unistd.h>

namespace sylar
{

// �߳���Ϣ   
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

// ��ȡ��ǰ�̵߳�����
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
    // �ȴ��̺߳�����ɳ�ʼ��
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

    /**  ΪʲôҪ������ִ��cb������ֱ��ִ��thread->m_cb
     * 1. ??�̰߳�ȫ����Դ����Ȩת��??
            ?? ���⾺̬����??��ֱ��ִ�� thread->m_cb() �����������߳��޸� m_cb �Ĳ�����ͻ���������߳����������̺߳��������� m_cb������������ͨ��ԭ���Ե�ת�� m_cb ������Ȩ���ֲ����� cb��ȷ���ص����������߳��ж�ռִ�У������������ա�
            ?? ��Դ�ͷſ���??��������thread->m_cb ��Ϊ�գ����߳̿ɰ�ȫ���� Thread ��������� m_cb�������赣�����߳����ڷ�������
        3. ??�߼����������������ڹ���??
            ?  ?��ȷִ�б߽�??�������󣬻ص����� cb ��ȫ�����߳̿��ƣ��� Thread ������������������������̳߳�ʼ���׶Σ��ź���֪ͨ��������ִ�н׶Σ�cb()���������߼����档
            ??  �쳣��ȫ??���� cb() �׳��쳣��������ľֲ����� cb ����ջչ��ʱ�Զ�������ȷ����Դ�ͷţ���ֱ��ִ�� m_cb() ����������Դδ����
     */
    std::function<void()> cb;
    cb.swap(thread->m_cb);

    // ��ʼ�����
    thread->m_semaphore.signal();

    cb();
    return 0;
}


}