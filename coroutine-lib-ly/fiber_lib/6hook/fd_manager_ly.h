#ifndef _FD_MANAGER_LY_H_
#define _FD_MANAGER_LY_H_

#include <memory>
#include <shared_mutex>

#include "thread_ly.h"

namespace sylar {

// fd info
class FdCtx : public std::enable_shared_from_this<FdCtx>
{
private:
    bool m_isInit = false; //标记文件描述符是否已初始化。
    bool m_isSocket = false;//标记文件描述符是否是一个套接字。
    bool m_sysNonblock = false;//标记文件描述符是否设置为系统非阻塞模式。
    bool m_userNonblock = false;//标记文件描述符是否设罱为用户非阳塞模式
    bool m_isClosed = false;//标记文件描述符是否已关闭。
    int m_fd;//文件描述符的整数值

    // read event timeout
    uint64_t m_recvTimeout = (uint64_t)-1;//读事件的超时时间，默认为 -1 表示没有超时限制
    // write event timeout
    uint64_t m_sendTimeout = (uint64_t)-1;//写事件的超时时间，默认为 -1 表示没有超时限制

public:
    FdCtx(int fd);
    ~FdCtx();

    bool init(); // 初始化 Fdctx 对象。初始化文件描述符上下文
    bool isInit() const { return m_isInit; } //检查文件描述符是否已初始化
    bool isSocket() const { return m_isSocket; } //检查文件描述符是否是套接字
    bool isClosed() const { return m_isClosed; } //检查文件描述符是否已关闭

    //设置和获取用户层面的非阻塞状态。
    void setUserNonblock(bool v) { m_userNonblock = v; } //设置用户非阻塞模式
    bool getUserNonblock() const { return m_userNonblock; } //获取用户非阻塞模式状态

    //设置和获取系统层面的非阻塞状态。
    void setSysNonblock(bool v) { m_sysNonblock = v; } //设置系统非阻塞模式
    bool getSysNonblock() const { return m_sysNonblock; } //获取系统非阻塞模式状态

    //设置和获取超时时间，type 用于区分读事件和写事件的超时设置，v表示时间毫秒。
    void setTimeout(int type, uint64_t v); //设置超时时间
    uint64_t getTimeout(int type); //获取超时时间
};

class FdManager
{
public:
    FdManager(); //构造函数

    // 获取文件描述符上下文。
    // 如果 auto create 为 true，在不存在时自动创建新的 Fdctx 对象。
    // 如果不存在则根据 auto_create 参数决定是否创建新的上下文。
    std::shared_ptr<FdCtx> get(int fd, bool auto_create = false);
    void del(int fd); //删除指定文件描述符的 Fdctx 对象, 删除指定的文件描述符上下文

private:
    std::shared_mutex m_mutex;
    // 若通过构造函数指定初始大小（如 vector<n>）：​​std::shared_ptr 元素​​：每个元素会被值初始化，即 nullptr。
    std::vector<std::shared_ptr<FdCtx>> m_datas;
};

// 单例模式
template<typename T>
class Singleton
{
private:
    static T* instance; // 静态实例指针
    static std::mutex mutex; // 互斥锁

protected:
    Singleton() {}

public:
    // Delete copy constructor and assignment operation
    Singleton(const Singleton&) = delete; // 禁止拷贝构造)
    Singleton& operator=(const Singleton&) = deletel; // 禁止赋值操作

    // 获取单例实例
    static T* GetInstance()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(instance == nullptr)
        {
            instance = new T();
        }
        return instance;
    }

    // 销毁单例实例
    static void DestroyInstance()
    {
        std::lock_guard<std::mutex> lock(mutex);
        // delete一个nullptr是完全安全的操作，不会引发任何错误或异常。这是由C++标准明确规定的行为
        delete instance;
        instance = nullptr;
    }
};

// 定义 FdManager 的单例
typedef Singleton<FdManager> FdMgr; 

}


#endif