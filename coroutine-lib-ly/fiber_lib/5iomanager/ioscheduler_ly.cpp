
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>

#include "ioscheduler_ly.h"

static bool debug = true;

namespace sylar {

IOManager* IOManager::GetThis()
{
    return dynamic_cast<IOManager*>(Scheduler::GetThis());
}

IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event)
{
    assert(event == READ || event == WRITE);

    switch(event)
    {
        case READ:
            return read;
        case WRITE:
            return write;
    }
    // std:invalid argument:属于std:exception的派生类
    // 在<stdexcept>头文件中，简单来说就是传入的参数不符合预期的参数时抛出异常。
    throw std::invalid_argument("Unsupported event type");
}

void IOManager::FdContext::resetEventContext(EventContext &ctx)
{
    // std::unique_lock<std::mutex> write_lock(mutex);

    ctx.cb = nullptr;
    ctx.fiber.reset();
    ctx.scheduler = nullptr;
}

// no lock
 void IOManager::FdContext::triggerEvent(IOManager::Event event)
 {
    assert(events & event);

    // delete event
    events = (Event)(events & ~event);

    // trigger
    EventContext& ctx = getEventContext(event);
    if(ctx.cb)
    {
        // call ScheduleTask(std::function<void()>* f, int thr)
        ctx.scheduler->scheduleLock(&ctx.cb);
    }
    else{
        // call ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
        ctx.scheduler->scheduleLock(&ctx.fiber);
    }

    // reset event context
    resetEventContext(ctx);
    return;
 }

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name):
Scheduler(threads, use_caller, name), TimerManager()
{
    // create epoll fd
    m_epfd = epoll_create(5000);
    assert(m_epfd > 0);

    // create pipe
    int rt = pipe(m_tickleFds); //创建管道的函数规定了m tickleFds[0]是读端，1是写段
    assert(!rt);//错误就终止程序

    // add read event to epoll
    epoll_event event;
    event.events = EPOLLIN | EPOLLET ; //Edge Triggered，设置标志位，并且采用边缘触发和读事件。
    event.data.fd = m_tickleFds[0];

    // non-blocked 若读取端无数据，read() 立即返回 -1，并设置 errno 为 EAGAIN 或 EWOULDBLOCK
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    assert(!rt);

    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    assert(!rt);

    contextResize(32);

    start();
}














}