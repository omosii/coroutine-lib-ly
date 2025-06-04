


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
    // �׳�һ�� std::invalid_argument ���͵��쳣������������Ϣ "Unsupported event type"��
    // �������ֹ��������µĴ�����Ϣ
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