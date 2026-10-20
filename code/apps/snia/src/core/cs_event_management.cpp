// SNIA event：完成队列由标准容器管理，无需手工维护链表和节点释放。
#include "cs_internal.hpp"
using namespace snia;
CS_STATUS csCreateEvent(CS_EVT_HANDLE *out)
{
    return apiCall([&] {
        if (!out) return CS_INVALID_ARG;
        *out = nullptr;
        *out = publishHandle(std::make_shared<Event>());
        return CS_SUCCESS;
    });
}
CS_STATUS csDeleteEvent(CS_EVT_HANDLE handle)
{
    return apiCall([&] {
        auto event = acquireHandle<Event>(handle);
        if (!event) return CS_INVALID_HANDLE;
        CloseHandle closing(event);
        if (!closing) return CS_HANDLE_IN_USE;
        closing.commit();
        return CS_SUCCESS;
    });
}
CS_STATUS csPollEvent(CS_EVT_HANDLE handle, void **context, u64 *value)
{
    return apiCall([&] {
        auto event = acquireHandle<Event>(handle);
        if (!event) return CS_INVALID_HANDLE;
        std::lock_guard<std::mutex> lock(handlesMutex);
        if (event->completions.empty()) return CS_NOT_DONE;
        auto completion = event->completions.front();
        event->completions.pop_front();
        if (context) *context = completion.context;
        if (value) *value = completion.value;
        return completion.status;
    });
}
