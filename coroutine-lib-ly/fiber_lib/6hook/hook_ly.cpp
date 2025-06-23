#include "hook_ly.h"
#include "ioscheduler.h"
#include <dlfcn.h>
#include <iostream>
#include <cstdarg>
#include "fd_manager.h"
#include <string.h>

// apply XX to all functions
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt) 

namespace sylar{

// if this thread is using hooked function 
// 不启用钩子函数时，sleep为原始系统调用，启用后为自定义调用
// 此时sleep_f为原始系统调用
static thread_local bool t_hook_enable = false;

bool is_hook_enable()
{
    return t_hook_enable;
}

void set_hook_enable(bool flag)
{
    t_hook_enable = flag;
}

void hook_init()
{
    static bool is_inited = false;
    if(is_inited)
    {
        return;
    }

    // test
    is_inited = true;


// assignment -> sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); -> dlsym -> fetch the original symbols/function
// XX(name)会被替换为name_f = (name_fun)dlsym(RTLD_NEXT, #name);。
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
    HOOK_FUN(XX)
#undef XX
}

// static variable initialisation will run before the main function
struct HookIniter
{
	HookIniter()
	{
		hook_init();//初始化hook，让原始调用绑定到宏展开的函数指针中
	}
};

static HookIniter s_hook_initer;//定义了一个静态的 HookIniter 实例。由于静态变量的初始化发生在 main()函数之前，所以 hook init()会在程序开始时被调用，从而初始化钩子函数

} // end namespace sylar

// 用于跟踪定时器的状态。具体来说，它有一个cancelled成员变量，通常用于表示定时器是否已经被取消。
struct timer_info 
{
    int cancelled = 0;
};

// universal template for read and write function
template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name, uint32_t event, int timeout_so, Args&&... args)
{
    if(!sylar::t_hook_enable) 
    {  
        // //如果全局钩子功能未启用，则直接调用原始的 I/0 函数
        return fun(fd, std::forward<Args>(args)...);
    }

    // //获取与文件描述符 fd 相关联的上下文 ctx。如果上下文不存在，则直接调用原始的 I/0 函数。
    //typedef singleton<FdManager>FdMgr各位彦祖不要忘记了。
    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
    if(!ctx) 
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    //如果文件描述符已经关闭，设置errno为EBADF并返回-1.
    if(ctx->isClosed()) 
    {
        errno = EBADF; //表示文件描述符无效或已经关闭
        return -1;
    }

    // 如果文件描述符不是一个socket或者用户设置了非阻塞模式，则直接调用原始的I/0操作函数
    if(!ctx->isSocket() || ctx->getUserNonblock()) 
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // get the timeout
    //获取超时设置并初始化timer info结构体，用于后续的超时管理和取消操作。
    uint64_t timeout = ctx->getTimeout(timeout_so);
    // timer condition
    std::shared_ptr<timer_info> tinfo(new timer_info);

//调用原始的I/0函数，如果由于系统中断(EINTR)导致操作失败，函数会重试。
retry:
    // run the function
    ssize_t n = fun(fd, std::forward<Args>(args)...);

    // EINTR ->Operation interrupted by system ->retry
    while(n == -1 && errno == EINTR) 
    {
        n = fun(fd, std::forward<Args>(args)...);
    }

    // 0 resource was temporarily unavailable -> retry until ready 
    //如果I/0操作因为资源暂时不可用(EAGAIN)而失败，函数会添加一个事件监听器来等待资源可用。同时，如果有超时设置，还会启动一个条件计时器来取消事件
    if(n == -1 && errno == EAGAIN) 
    {
        sylar::IOManager* iom = sylar::IOManager::GetThis();
        // timer
        std::shared_ptr<sylar::Timer> timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        // 1 timeout has been set -> add a conditional timer for canceling this operation
        //如果执行的read等函数在Fdmanager管理的Fdctx中fd设置了超时时间，就会走到这里。添加addconditionTimer事件
        if(timeout != (uint64_t)-1) 
        {
            timer = iom->addConditionTimer(timeout, [winfo, fd, iom, event]() 
            {
                auto t = winfo.lock();
                //如果 timer info 对象已被释放(!t)，或者操作已被取消(t->cancelled 非 )，则直接返回,
                if(!t || t->cancelled) 
                {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                // cancel this event and trigger once to return to this fiber
                iom->cancelEvent(fd, (sylar::IOManager::Event)(event));
            }, winfo);
        }
    }

}
















/**
 * 注1：
 * 若调用 HOOK_FUN(XX) 并传入上述 XX 宏，则完整展开后代码如下：
 * sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep");
    usleep_f = (usleep_fun)dlsym(RTLD_NEXT, "usleep");
    nanosleep_f = (nanosleep_fun)dlsym(RTLD_NEXT, "nanosleep");
    socket_f = (socket_fun)dlsym(RTLD_NEXT, "socket");
    connect_f = (connect_fun)dlsym(RTLD_NEXT, "connect");
    accept_f = (accept_fun)dlsym(RTLD_NEXT, "accept");
    read_f = (read_fun)dlsym(RTLD_NEXT, "read");
    readv_f = (readv_fun)dlsym(RTLD_NEXT, "readv");
    recv_f = (recv_fun)dlsym(RTLD_NEXT, "recv");
    recvfrom_f = (recvfrom_fun)dlsym(RTLD_NEXT, "recvfrom");
    recvmsg_f = (recvmsg_fun)dlsym(RTLD_NEXT, "recvmsg");
    write_f = (write_fun)dlsym(RTLD_NEXT, "write");
    writev_f = (writev_fun)dlsym(RTLD_NEXT, "writev");
    send_f = (send_fun)dlsym(RTLD_NEXT, "send");
    sendto_f = (sendto_fun)dlsym(RTLD_NEXT, "sendto");
    sendmsg_f = (sendmsg_fun)dlsym(RTLD_NEXT, "sendmsg");
    close_f = (close_fun)dlsym(RTLD_NEXT, "close");
    fcntl_f = (fcntl_fun)dlsym(RTLD_NEXT, "fcntl");
    ioctl_f = (ioctl_fun)dlsym(RTLD_NEXT, "ioctl");
    getsockopt_f = (getsockopt_fun)dlsym(RTLD_NEXT, "getsockopt");
    setsockopt_f = (setsockopt_fun)dlsym(RTLD_NEXT, "setsockopt");
 * */