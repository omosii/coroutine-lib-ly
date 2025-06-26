#include "hook_ly.h"
#include "ioscheduler_ly.h"
#include <dlfcn.h>
#include <iostream>
#include <cstdarg>
#include "fd_manager_ly.h"
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
static thread_local bool t_hook_enable = true;

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

        // 2 add event -> callback is this fiber
        // 这行代码的作用是将 fd(文件描述符)和 event(要监听的事件，如读或写事件)添加到 I0Manager 中进行管理。I0Manager 会监听这个文件描述符上的事件。当事件触发，调度相应协程处理事件。
        int rt = iom->addEvent(fd, (sylar::IOManager::Event)(event));
        if(rt) 
        {
            //如果 rt 为-1，说明 addEvent 失败。此时，会打印一条调试信息，并且因为添加事件失败所以要取消之前设置的定时器，避免误触发。
            std::cout << hook_fun_name << " addEvent("<< fd << ", " << event << ")";
            if(timer) 
            {
                timer->cancel();
            }
            return -1;
        } 
        else 
        {
            //如果 addEvent 成功(rt为0)，当前协程会调用 yield()函数，将自己挂起，等待事件的触发。
            sylar::Fiber::GetThis()->yield();
     
            // 3 resume either by addEvent or cancelEvent
            // 当协程被恢复时(例如，事件触发后)，它会继续执行yield()之后的代码。
            // 如果之前设置了定时器(timer 不为 nullptr)，则在事件处理完毕后取消该定时器。取消定时器的原因是，该定时器的唯一目的是在 I/0 操作超时时取消事件。 如果事件正常处理完毕，那么就不再需要定时器了。
            if(timer) 
            {
                timer->cancel();
            }
            // by cancelEvent
            //接下来检査 tinfo->cancelled 是否等于 ETIMEDOUT。如果等于，说明该操作因超时而被取消，因此设置errno为 ETIMEDOUT 并返回 -1，表示操作失败
            if(tinfo->cancelled == ETIMEDOUT) 
            {
                errno = tinfo->cancelled;
                return -1;
            }
            //如果没有超时，则跳转到 retry 标签，重新尝试这个操作。
            goto retry;
        }
    }
    return n;
}

extern "C"{
// declaration -> sleep_fun sleep_f = nullptr;
#define XX(name) name ## _fun name ## _f = nullptr;
    HOOK_FUN(XX)    
#undef XX

// only use at task fiber
unsigned int sleep(unsigned int seconds)
{
    //如果钩子未启用，则调用原始的系统调用
	if(!sylar::t_hook_enable)
	{
		return sleep_f(seconds);
	}
    //获取当前正在执行的协程(Fiber)，并将其保存到fiber 变量中。
	std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
    
	sylar::IOManager* iom = sylar::IOManager::GetThis();
	// add a timer to reschedule this fiber
	iom->addTimer(seconds*1000, [fiber, iom](){iom->scheduleLock(fiber, -1);});
	// wait for the next resume
    // 挂起当前协程的执行，将控制权交还给调度器。
	fiber->yield();
	return 0;
}

int usleep(useconds_t usec)
{
	if(!sylar::t_hook_enable)
	{
		return usleep_f(usec);
	}

	std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
	sylar::IOManager* iom = sylar::IOManager::GetThis();
	// add a timer to reschedule this fiber
	iom->addTimer(usec/1000, [fiber, iom](){iom->scheduleLock(fiber);});
	// wait for the next resume
	fiber->yield();
	return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem)
{
	if(!sylar::t_hook_enable)
	{
		return nanosleep_f(req, rem);
	}	
    //timeout ms 将 tv sec 转换为毫秒，并将 tv nsec 转换为毫秒，然后两者相加得到总的超时毫秒数。所以从这里看出实现的也是一个毫秒级的操作。
	int timeout_ms = req->tv_sec*1000 + req->tv_nsec/1000/1000;

	std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
	sylar::IOManager* iom = sylar::IOManager::GetThis();
	// add a timer to reschedule this fiber
	iom->addTimer(timeout_ms, [fiber, iom](){iom->scheduleLock(fiber, -1);});
	// wait for the next resume
	fiber->yield();	
	return 0;
}

int socket(int domain, int type, int protocol)
{
	if(!sylar::t_hook_enable)
	{
		return socket_f(domain, type, protocol);
	}	
    // 如果钩子启用了，则通过调用原始的 socket 函数创建套接字，并将返回的文件描述符存储在 fd 变量中
	int fd = socket_f(domain, type, protocol);
	if(fd==-1)// fd是无效的情况
	{
		std::cerr << "socket() failed:" << strerror(errno) << std::endl;
		return fd;
	}
    // 如果socket创建成功会利用Fdmanager的文件描述符管理类来进行管理，判断是否在其管理的文件描述符中，如果不在扩展存储文件描述数组大小，并且利用FDctx进行初始化判断是不是套接字，是不是系统非阻塞模式。
	sylar::FdMgr::GetInstance()->get(fd, true);
	return fd;
}

int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms) 
{
    if(!sylar::t_hook_enable) 
    {
        return connect_f(fd, addr, addrlen);
    }

    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd); //获取文件描述符 fd 的上下文信息 Fdctx
    if(!ctx || ctx->isClosed()) //检查文件描述符上下文是否存在或是否已关闭。
    {
        errno = EBADF;//EBAD表示一个无效的文件描述符
        return -1;
    }
    //如果不是一个套接字调用原始的
    if(!ctx->isSocket()) 
    {
        return connect_f(fd, addr, addrlen);
    }
    //检查用户是否设置了非阻塞模式。如果是非阻塞模式，
    if(ctx->getUserNonblock()) 
    {

        return connect_f(fd, addr, addrlen);
    }

    // attempt to connect
    //尝试进行 connect 操作，返回值存储在 n 中。
    int n = connect_f(fd, addr, addrlen); 
    if(n == 0) // 连接立即成功​
    {
        return 0;
    } 
    else if(n != -1 || errno != EINPROGRESS) // 连接明确失败​
    {
        return n;
    }
    // 下面是 n = -1 && errno == EINPROGRESS 的情况
    // 若 connect() 返回 -1 且 errno == EINPROGRESS，表示连接​​正在后台进行三次握手​​（非阻塞模式下常见）。连接未立即完成（返回EINPROGRESS）

    // wait for write event is ready -> connect succeeds
    sylar::IOManager* iom = sylar::IOManager::GetThis(); //获取当前线程的 IOManager 实例。
    std::shared_ptr<sylar::Timer> timer; //声明一个定时器对象。
    std::shared_ptr<timer_info> tinfo(new timer_info); //创建追踪定时器是否取消的对象
    std::weak_ptr<timer_info> winfo(tinfo); //判断追踪定时器对象是否存在
    // //检査是否设置了超时时间。如果 timeout ms 不等于 -1，则创建一个定时器。
    if(timeout_ms != (uint64_t)-1) 
    {
        // 添加一个定时器，当超时时间到达时，取消事件监听并设置 cancelled 状态。
        timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]() 
        {
            //判断追踪定时器对象是否存在或者追踪定时器的成员变量是否大于0.大于0就意味着取消了
            auto t = winfo.lock();
            if(!t || t->cancelled) 
            {
                return;
            }
            t->cancelled = ETIMEDOUT; //如果超时了但时间任然未处理
            iom->cancelEvent(fd, sylar::IOManager::WRITE); //将指定的fd的事件触发将事件处理
        }, winfo);
    }

    //为文件描述符 fd 添加一个写事件监听器。这样的目的是为了上面的回调函数处理指定文件描述符
    int rt = iom->addEvent(fd, sylar::IOManager::WRITE);
    if(rt == 0)  //代表添加事件成功
    {
        sylar::Fiber::GetThis()->yield();

        // resume either by addEvent or cancelEvent
        if(timer) //如果有定时器，取消定时器。
        {
            timer->cancel();
        }

        if(tinfo->cancelled) //发生超时错误或者用户取消
        {
            errno = tinfo->cancelled;
            return -1;
        }
    } 
    else //添加事件失败
    {
        if(timer) //如果有定时器，取消定时器。
        {
            timer->cancel();
        }
        std::cerr << "connect addEvent(" << fd << ", WRITE) error";
    }

    // check out if the connection socket established 
    // getsockopt 通过 SO_ERROR 选项获取套接字上未处理的错误码（例如非阻塞 connect() 或异步操作中的错误），并​​清除​​该错误状态（类似于读取后复位）
    int error = 0;
    socklen_t len = sizeof(int);
    if(-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) //通过getsocketopt检查套接字实际错误状态
    {
        return -1;
    }
    if(!error) //如果没有错误，返回0 表示连接成功，
    {
        return 0;
    } 
    else //如果有错误，设置 errno 并返回错误。
    {
        errno = error; // errno是线程局部的
        return -1;
    }
}

static uint64_t s_connect_timeout = -1;
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	int fd = do_io(sockfd, accept_f, "accept", sylar::IOManager::READ, SO_RCVTIMEO, addr, addrlen);	
	if(fd>=0)
	{
		sylar::FdMgr::GetInstance()->get(fd, true);
	}
	return fd;
}
/*
socketfd:监听套接字的文件描述符,
accept f:原始的accpet系统调用函数指针。
"accpet":操作名称，用于调试和日志记录。
sylar::lOManager::READ:表示READ事件(即有新的连接可用时触发)。
SO RCVTIMEO:接收超时时间选项，用于处理超时逻辑。
addr和addrlen:用于保存新链接的客户端地址信息。
*/


/*
​* ​read​​	从文件描述符（包括套接字）读取数据到单个缓冲区	通用文件/套接字读取	无额外控制参数，行为简单，适用于连续数据块读取。
​​* ​readv​​	从文件描述符读取数据到多个分散的缓冲区（分散读）	需批量读取非连续内存的场景	减少系统调用次数，支持原子操作（如UDP单数据报）。
​​* ​recv​​	从套接字读取数据，支持附加控制标志（如MSG_PEEK）	TCP/UDP套接字	提供flags参数控制行为（如预览数据、非阻塞等）。
​​* ​recvfrom​​	从套接字读取数据并获取发送方地址（主要用于无连接协议）	UDP套接字	比recv多src_addr参数，适用于需获取源地址的场景。
​​* ​recvmsg​​	最灵活的接收函数，支持多缓冲区、控制消息（如文件描述符传递）	高级网络编程（如UNIX域套接字）	通过msghdr结构体支持分散读、辅助数据（如带外数据）。
*/
ssize_t read(int fd, void *buf, size_t count)
{
	return do_io(fd, read_f, "read", sylar::IOManager::READ, SO_RCVTIMEO, buf, count);	
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, readv_f, "readv", sylar::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);	
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
	return do_io(sockfd, recv_f, "recv", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags);	
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
	return do_io(sockfd, recvfrom_f, "recvfrom", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);	
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	return do_io(sockfd, recvmsg_f, "recvmsg", sylar::IOManager::READ, SO_RCVTIMEO, msg, flags);	
}

ssize_t write(int fd, const void *buf, size_t count)
{
	return do_io(fd, write_f, "write", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, count);	
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, writev_f, "writev", sylar::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);	
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
	return do_io(sockfd, send_f, "send", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags);	
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
	return do_io(sockfd, sendto_f, "sendto", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);	
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
	return do_io(sockfd, sendmsg_f, "sendmsg", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, flags);	
}

int close(int fd)
{
	if(!sylar::t_hook_enable)
	{
		return close_f(fd);
	}	

	std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

	if(ctx)
	{
		auto iom = sylar::IOManager::GetThis();
		if(iom)
		{	
			iom->cancelAll(fd);
		}
		// del fdctx
		sylar::FdMgr::GetInstance()->del(fd);
	}
	return close_f(fd);
}

/**
 * fcntl是一个用于操作文件描述符的系统调用，可以执行多种操作，比如设置文件描述符状态，锁定文件等。
 * 这个封装的fcntl函数对某些操作进行自定义处理，比如处理非阻塞模式表示，同时保留了对原始fcntl的调用。
 */
int fcntl(int fd, int cmd, ... /* arg */ )
{
  	va_list va; // to access a list of mutable parameters

    va_start(va, cmd); //使其指向第一个可变参数(在cmd 之后的参数)。
    switch(cmd) 
    {
        case F_SETFL:
            {
                int arg = va_arg(va, int); // Access the next int argument
                va_end(va);
                std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
                {
                    return fcntl_f(fd, cmd, arg);
                }
                // 用户是否设定了非阻塞
                ctx->setUserNonblock(arg & O_NONBLOCK);
                // 最后是否阻塞根据系统设置决定
                if(ctx->getSysNonblock()) 
                {
                    arg |= O_NONBLOCK;
                } 
                else 
                {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_f(fd, cmd, arg);
            }
            break;

        case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd);
                std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
                {
                    return arg;
                }
                // 这里是呈现给用户 显示的为用户设定的值
                // 但是底层还是根据系统设置决定的
                if(ctx->getUserNonblock()) 
                {
                    return arg | O_NONBLOCK;
                } else 
                {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;

        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int);
                va_end(va);
                return fcntl_f(fd, cmd, arg); 
            }
            break;


        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                va_end(va);
                return fcntl_f(fd, cmd);
            }
            break;

        case F_SETLK: //设置文件锁，如果不能立即获得锁，则返回失败。
        case F_SETLKW: //设置文件锁，且如果不能立即获得锁，则阻塞等待。
        case F_GETLK: 
            {
                //从可变参数列表中获取 struct f1ock* 类型的指针，这个指针指向一个 f1ock 结构体，包含锁定操作相关的信息(如锁的类型、偏移量、锁的长度等
                struct flock* arg = va_arg(va, struct flock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

        case F_GETOWN_EX: //获取文件描述符 fd 所属的所有者信息。这通常用于与信号处理相关的操作，尤其是在异步 I/0 操作中
        case F_SETOWN_EX: //设置文件描述符 fd 的所有者信息。
            {
                struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*); //从可变参数中提取相应类型的结构体指针
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }	
}

// 实际处理了文件描述符(fd)上的ioctl系统调用，并在特定条件下对FIONBIO(用于设置非阻塞模式)进行了特殊处理。
// ioctl（Input/Output Control）是Linux系统中用于设备控制的系统调用, 允许用户空间程序与内核或设备驱动程序交互，执行设备特定的操作（如配置串口波特率、获取网络接口状态等）
int ioctl(int fd, unsigned long request, ...)
{
    va_list va; // va持有处理可变参数的状态信息
    va_start(va, request); //给va初始化让它指向可变参数的第一个参数位置。
    void* arg = va_arg(va, void*); //将va的指向参数的以void*类型取出存放到arg中
    va_end(va); //用于结束对 va list 变量的操作。清理va占用的资源

    if(FIONBIO == request) //用于设置非阻塞模式的命令
    {
        bool user_nonblock = !!*(int*)arg; //当前 ioctl 调用是为了设置或清除非阻塞模式
        std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
        //检查获取的上下文对象是否有效(即 ctx 是否为空)。如果上下文对象无效、文件描述符已关闭或不是一个套接字，则直接调用原始的 ioctl
        if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
        {
            return ioctl_f(fd, request, arg);
        }
        //如果上下文对象有效，调用其 setuseronblock 方法，将非阻塞模式设置为 user nonblock 指定的值。这将更新文件描述符的非阻塞状态
        ctx->setUserNonblock(user_nonblock);
    }
    // 通过 ioctl(fd, FIONBIO, &arg)，将 arg 设为 1 时，文件描述符 fd 变为非阻塞模式；设为 0 时恢复为阻塞模式
    return ioctl_f(fd, request, arg);
}

// 一个用于获取套接字选项值的函数。它允许你检查指定套接字的某些选项的当前设置。
/**
 * 参数说明:
 * sockfd:这是一个套接字描述符，表示要操作的套接字,
 * level:指定选项的协议层次，常见的值是'SOL_SOCKET'，表示通用套接字选项，还有可能是'IPPROTO_TCP·等协议相关选项。
 * optname:表示你要获取的选项的名称。例如，可以是'SO_RCVBUF'，表示接收缓冲区大小或'SO_RESUEADDR'，表示地址重用。
 * optval:指向一个缓冲区，该缓冲区将存储选项的值
 * optlen:指向一个变量，该变量指定“optva"缓冲区的大小，并在函数返回时包含实际存储的选项值的大小。
 */
int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
	return getsockopt_f(sockfd, level, optname, optval, optlen);
}


/**
 * 用于设置套接字的选项。它允许你对套接字的行为进行配置，如设置超时时间、缓冲区大小、地址重用等。
 */
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    if(!sylar::t_hook_enable) 
    {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }

    //如果 level 是 SOL SOCKET 月 optname是 SO RCVTIMEO(接收超时)或 SO SNDTIMEO(发送超时)，代码会获取与该文件描述符关联的 Fdctx 上
    if(level == SOL_SOCKET) 
    {
        if(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) 
        {
            std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(sockfd);
            if(ctx) 
            {
                //那么代码会读取传入的 timeval 结构体，将其转化为毫秒数，并调用 ctx->setimeout 方法，记录超时设置
                // timeval结构体: 通常用于表示时间间隔，它在Unix系统中非常常见，定义如下:
                const timeval* v = (const timeval*)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    //无论是否执行了超时处理，最后都会调用原始的 setsockopt f 函数来设置实际的套接字选项
    return setsockopt_f(sockfd, level, optname, optval, optlen);	
}

// 总的来说setsockopt函数就是用来设置套接字，getsockopt就是用来查询套接字的状态和配置信息(在SOL SOCKET的情况下，检查超时时间设置，缓冲区大小，TCP协议选项等)。
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