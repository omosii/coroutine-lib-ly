#include "timer_ly.h"

namespace sylar{

bool Timer::cancel()
{
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

    if(m_cb == nullptr)
    {
        return false;
    }
    else{
        m_cb = nullptr;
    }

    auto it = m_manager->m_timers.find(shared_from_this());
    if(it != m_manager->m_timers.end())
    {
        m_manager->m_timers.erase(it);
    }

    return true;
}

// refresh 只会向后调整时间
bool Timer::refresh()
{
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

    if(!m_cb){
        return false;
    }

    auto it = m_manager->m_timers.find(shared_from_this());
    if(m_manager->m_timers.end() == it)
    {
        return false;
    }

    m_manager->m_timers.erase(it);
    m_next = std::chrono::system_clock::now() + 
                std::chrono::milliseconds(m_ms);
    m_manager->m_timers.insert(shared_from_this());

    return true;
}

bool Timer::reset(uint64_t ms, bool from_now)
{
    if(ms == m_ms && from_now == false)
    {
        return true;
    }

    {
        std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);
        
        if(!m_cb)
        {
            return false;
        }
        
        auto it = m_manager->m_timers.find(shared_from_this());
        if(it == m_manager->m_timers.end())
        {
            return false;
        }
        m_manager->m_timers.erase(it);
    }
    // reinsert
    auto start = from_now ? std::chrono::system_clock::now() : m_next - std::chrono::milliseconds(m_ms);
    m_next = start + std::chrono::milliseconds(ms);
    m_ms = ms;
    m_manager->addTimer(shared_from_this()); // insert with lock

    return true;
}







}