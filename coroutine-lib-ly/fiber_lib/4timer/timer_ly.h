#ifndef __SYLAR_TIMER_H__
#define __SYLAR_TIMER_H__

#include <memory>

namespace sylar
{

class TimerManager;

class Timer : public std::enable_shared_from_this<Timer>
{
    friend class TimerManager;
public:
    // 从时间堆中删除timer
    bool cancel();
    // 刷新timer
    bool refresh();
    // 重设timer的超时时间
    bool reset(uint64_t ms, bool from_now);

private:

};










}



#endif