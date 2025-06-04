


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
    // 抛出一个 std::invalid_argument 类型的异常，附带错误信息 "Unsupported event type"。
    // 程序会终止并输出以下的错误信息
    throw std::invalid_argument("Unsupported event type");
}

void IOManager::FdContext::resetEventContext(EventContext &ctx)
{
    // std::unique_lock<std::mutex> write_lock(mutex);

    ctx.cb = nullptr;
    ctx.fiber.reset();
    ctx.scheduler = nullptr;
}


















}