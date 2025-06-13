#include "fd_manager_ly.h"
#include "hook.h" // Fix me

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sylar{

// instantiate
template class Singleton<FdManager>;// FdManager 类有一个全局唯一的单例实例。

// Static variables need to be defined outside the class
// 这些行代码定义了 singleton 类模板的静态成员变量 instance 和 mutex。静态成员变量需要在类外部定义和初始化。
template<typename T>
T* Singleton<T>::instance = nullptr; // 静态实例指针初始化为 nullptr
template<typename T>
std::mutex Singleton<T>::mutex; // 静态互斥锁初始化

FdCtx::FdCtx(int fd) : m_fd(fd)
{
    init(); // 
}

FdCtx::~FdCtx()
{

}

bool FdCtx::init()
{
    if(m_isInit)
	{
		return true;
	}

    struct stat statbuf; // 文件的元数据（metadata），如文件大小、权限、类型、时间戳等
	// fd is in valid
	if(-1 == fstat(m_fd, &statbuf)) // fstat 通过已打开的文件描述符（file descriptor）获取文件的元数据。
	{
		m_isInit = false;
		m_isSocket = false;
	}
	else
	{
		m_isInit = true;	
		m_isSocket = S_ISSOCK(statbuf.st_mode);	
	}

    // if it is a socket -> set to nonblock
    if(m_isSocket)
    {
        // fcntl_f() -> the original fcntl() -> get the socket info
        int flags = fcntl_f(m_fd, F_GETFL, 0); // 获取文件描述符的标志
        if(!(flags & O_NONBLOCK)) // 检查是否已经是非阻塞模式
        {
            // if not -> set to nonblock
            fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK); // 设置为非阻塞模式
        }
        m_sysNonblock = true; // 系统非阻塞模式标记为 true
    }
    else
    {
        m_sysNonblock = false; // 如果不是套接字，则系统非阻塞模式标记为 false
    }

    return m_isInit;

}

void FdCtx::setTimeout(int type, uint64_t v)
{
    if(type == SO_RCVTIMEO)
    {
        // SO_RCVTIMEO 是 Linux 网络编程中的一个 ​​套接字选项​​（socket option），用于设置套接字在接收数据时的超时时间。
        m_recvTimeout = v; // 设置接收超时时间
    }
    else
    {
        m_sendTimeout = v; // 设置发送超时时间
    }
}

uint64_t FdCtx::getTimeout(int type)
{
    if(type == SO_RCVTIMEO)
    {
        return m_recvTimeout; // 返回接收超时时间
    }
    else
    {
        return m_sendTimeout; // 返回发送超时时间
    }
}

FdManager::FdManager()
{
    m_datas.resize(64); // 初始化文件描述符上下文数组，大小为 64
}

std::shared_ptr<FdCtx> FdManager::get(int fd, bool auto_create)
{
    if(fd < 0) // if(fd==-1)
    {
        return nullptr; // 如果文件描述符无效，返回空指针
    }

    std::shared_lock<std::shared_mutex> read_lock(m_mutex); // 共享锁，允许多个线程同时读取
    if(m_datas.size() <= fd) // 如果文件描述符超过当前数组大小
    {
        if(auto_create == false) // 如果不允许自动创建
        {
            return nullptr; // 返回空指针
        }
    }
    else
    {
        if(m_datas[fd] || !auto_create) // 如果文件描述符上下文已存在或不允许自动创建
        {
            return m_datas[fd]; // 返回已存在的文件描述符上下文
        }
    }

    read_lock.unlock(); // 释放共享锁
    std::unique_lock<std::shared_mutex> write_lock(m_mutex); // 独占锁，允许一个线程写入
    if(m_datas.size() <= fd) // 如果文件描述符超过当前数组大小
    {
        m_datas.resize(fd * 1.5); // 扩展数组大小
    }

    m_datas[fd] = std::make_shared<FdCtx>(fd); // 创建新的文件描述符上下文并存储在数组中
    return m_datas[fd]; // 返回新创建的文件描述符上下文

}











}

