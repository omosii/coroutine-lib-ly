#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

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

    // 将读取端添加到epoll中, 读端怎么从管道里读数据呢
    // 触发后在idle中读取
    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    assert(!rt);

    contextResize(32);

    start();
}

IOManager::~IOManager()
{
    stop();// 关闭scheduler类中的线程池，让任务全部执行完后线程安全退出
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);
    // 将fdcontext文件描述符一个个关闭
    for(size_t i = 0; i < m_fdContexts.size(); ++i)
    {
        if(m_fdContexts[i])
        {
            delete m_fdContexts[i];
        }
    }
}

// no lock
void IOManager::contextResize(size_t size) 
{
    m_fdContexts.resize(size);

    for(size_t i = 0; i < m_fdContexts.size(); ++i)
    {
        if(m_fdContexts[i] == nullptr)
        {
            m_fdContexts[i] = new FdContext();
            m_fdContexts[i]->fd = i; // fdcontext的fd是索引
        }
    }
}

int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
{
    // attemp to find FdContext 
    FdContext* fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fdContexts.size() > fd)
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else
    {
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        contextResize(fd * 1.5); // 扩容
        fd_ctx = m_fdContexts[fd];
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event has already been added
    if(fd_ctx->events & event)
    {
        return -1; // event already exists
    }

    // add the new event
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD; // 如果fd_ctx->events不为0，说明已经有事件了，就修改，否则就添加
    epoll_event epevent;
    epevent.events = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt)
    {
        std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
    }

    ++m_pendingEventCount;

    // update fdcontext
    fd_ctx->events = (Event)(fd_ctx->events | event);

    // update event context
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);//确保 EventContext 中没有其他正在执行的调度器、协程或回调函数
    event_ctx.scheduler = Scheduler::GetThis();
    // 如果提供了回调函数 cb，则将其保存到 Eventcontext 中;否则，将当前正在运行的协程保存到 Eventcontext 中，并确保协程的状态是正在运行。
    if(cb)
    {
        event_ctx.cb.swap(cb);
    }
    else
    {
        event_ctx.fiber = Fiber::GetThis();
        assert(event_ctx.fiber->getState() == Fiber::RUNNING);
    }
    return 0; // success
}

bool IOManager::delEvent(int fd, Event event)
{
    // attemp to find FdContext 
    FdContext *fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fdContexts.size() > fd)
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event doesn't exist
    if(!(fd_ctx->events & event))
    {
        return false; // event not found
    }

    // delete the event
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL; // 如果还有其他事件，就修改，否则就删除
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt)
    {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return false;
    }

    --m_pendingEventCount;

    // update fdcontext
    fd_ctx->events = new_events;

    // update event context 
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx); // reset event context

    return true; // success
}

bool IOManager::cancelEvent(int fd, Event event)
{
    // attemp to find FdContext 
    FdContext *fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fdContexts.size() > fd)
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event doesn't exist
    if(!(fd_ctx->events & event))
    {
        return false; // event not found
    }

    // delete the event
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL; // 如果还有其他事件，就修改，否则就删除
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt)
    {
        std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return false;
    }

    --m_pendingEventCount;

    // update fdcontext, event context and trigger
    fd_ctx->triggerEvent(event);    

    return true; // success
}

bool IOManager::cancelAll(int fd)
{
    // attemp to find FdContext 
    FdContext *fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    
    // none of events exist
    if (!fd_ctx->events) 
    {
        return false;
    }

    // delete all events
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = 0;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "IOManager::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return false;
    }

    // update fdcontext, event context and trigger
    for (Event event : {READ, WRITE}) 
    {
        if (fd_ctx->events & event) 
        {
            fd_ctx->triggerEvent(event);
            --m_pendingEventCount;
        }
    }

    assert(fd_ctx->events == 0);

    return true;
}

void IOManager::tickle()
{
    // no idle threads
    if(!hasIdleThreads()) 
    {
        return;
    }

    // write to the write end of the pipe
    int rt = write(m_tickleFds[1], "T", 1);
    assert(rt == 1);
}

bool IOManager::stopping()
{
    uint64_t timeout = getNextTimer();
    // no timers left and no pending events left with the Scheduler::stopping()
    return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}

void IOManager::idle()
{
    static const uint64_t MAX_EVNETS = 256;
    std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVNETS]);

    while (true) 
    {
        if(debug) std::cout << "IOManager::idle(), run in thread: " << Thread::GetThreadId() << std::endl; 

        if(stopping()) 
        {
            if(debug) std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;
            break;
        }

        // blocked at epoll_wait
        int rt = 0;
        while(true) // 超时或有事件触发，跳出while
        {
            static const uint64_t MAX_TIMEOUT = 5000;
            uint64_t next_timeout = getNextTimer();
            next_timeout = std::min(next_timeout, MAX_TIMEOUT);

            rt = epoll_wait(m_epfd, events.get(), MAX_EVNETS, (int)next_timeout);
            // EINTR -> retry
            if(rt < 0 && errno == EINTR) 
            {
                // 当epoll_wait返回值为-1且errno为EINTR时，表示系统调用被信号（如用户中断或其他异步事件）打断。这是一种可恢复的错误，因此代码通过continue重新尝试调用epoll_wait。
                continue;
            }
            else
            {
                // 若epoll_wait返回其他错误（非EINTR）或成功（返回值≥0），则通过break退出循环。成功时返回值表示就绪的文件描述符数量
                break;
            }
        }

        // collect all timers overdue
        // epoll_wait 会返回 0，表示超时且无事件发生
        std::vector<std::function<void()>> cbs;
        listExpiredCb(cbs);
        if(!cbs.empty()) 
        {
            for(const auto& cb : cbs) 
            {
                scheduleLock(cb);
            }
            cbs.clear();
        }

        // collect all events ready
        for (int i = 0; i < rt; ++i) 
        {
            epoll_event& event = events[i];

            // tickle event
            if (event.data.fd == m_tickleFds[0]) 
            {
                uint8_t dummy[256];
                // edge triggered -> exhaust
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }

            // other events
            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            std::lock_guard<std::mutex> lock(fd_ctx->mutex);

            // convert EPOLLERR or EPOLLHUP to -> read or write event
            // 当检测到 EPOLLERR（文件描述符错误）或 EPOLLHUP（连接挂断）时，代码会强制将当前文件描述符（fd）的 ​​可读（EPOLLIN）和可写（EPOLLOUT）事件​​ 添加到 event.events 中
            if (event.events & (EPOLLERR | EPOLLHUP)) 
            {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }
            // events happening during this turn of epoll_wait
            int real_events = NONE;
            if (event.events & EPOLLIN) 
            {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT) 
            {
                real_events |= WRITE;
            }
            if ((fd_ctx->events & real_events) == NONE) 
            {
                continue;
            }

            // delete the events that have already happened
            int left_events = (fd_ctx->events & ~real_events);
            int op          = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events    = EPOLLET | left_events;

            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (rt2) 
            {
                std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl; 
                continue;
            }

            // schedule callback and update fdcontext and event context
            if (real_events & READ) 
            {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if (real_events & WRITE) 
            {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        } // end for
        Fiber::GetThis()->yield();
    }
}

void IOManager::onTimerInsertedAtFront()
{
    // tickle the IOManager to wake up the idle fiber
    tickle();
}


}// end namespace sylar