#ifndef _COROUTINE_H_
#define _COROUTINE_H_

#include <iostream>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>
#include <ucontext.h>
#include <cassert>

namespace sylar {

class Fiber : public std::enable_shared_from_this<Fiber>
{
public:
    // 
    enum State{
        READY,
        RUNNING,
        TERM
    };

    std::mutex m_mutex;

private:
    //
    Fiber();

public:
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
    ~Fiber();

    //
    void reset(std::function<void()> cb);
    //
    void resume();
    void yield();

    uint64_t getId() const {return m_id;}
    State getState() const {return m_state;}
    //
    static void SetThis(Fiber *f);
    //
    static std::shared_ptr<Fiber> GetThis();
    //
    static void SetSchedulerFiber(Fiber *f);
    //
    static uint64_t GetFiberId();
    //
    static void MainFunc();

private:
    // 
    uint64_t m_id = 0;
    //
    State m_state = READY;
    //
    uint32_t m_stacksize = 0;
    // ucontext_t 
    ucontext_t m_ctx;
    //
    void* m_stack = nullptr;
    //
    std::function<void()> m_cb;
    //
    bool m_runInScheduler;



};


}

#endif