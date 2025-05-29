#include "scheduler_ly.h"

static bool debug = false;

namespace sylar{

static thread_local Scheduler* t_scheduler = nullptr;

// 获取正在运行的调度器
Scheduler* Scheduler::GetThis()
{
    return t_scheduler;
}
// 设置正在运行的调度器
void Scheduler::SetThis()
{
    t_scheduler = this;
}

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name):
m_useCaller(use_caller), m_name(name)
{
    assert(threads > 0 && Scheduler::GetThis == nullptr);

    SetThis();

    Thread::SetName(m_name);

    
}





















}