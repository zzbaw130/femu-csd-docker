// SNIA batch：标准容器保存配置草稿，锁守卫与线程对象管理调度生命周期。
#include "cs_common.hpp"
using namespace snia;
namespace {
bool acyclic(const std::array<BatchEntry, MaxBatch> &entries, unsigned capacity)
{
    u64 all = 0, done = 0;
    for (unsigned i = 0; i < capacity; ++i) if (entries[i].request) all |= u64{1} << i;
    for (unsigned pass = 0; pass < capacity; ++pass) {
        u64 previous = done;
        for (unsigned i = 0; i < capacity; ++i)
            if ((all & (u64{1} << i)) && !(entries[i].dependencies & ~done)) done |= u64{1} << i;
        if (done == all) return true;
        if (done == previous) return false;
    }
    return false;
}
class BatchExecution final {
    const Batch &batch;
    std::atomic_bool &cancelled;
    std::mutex mutex;
    std::condition_variable changed;
    u64 all = 0, started = 0, done = 0, value = 0;
    unsigned running = 0;
    CS_STATUS error = CS_SUCCESS;
    void worker()
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (error == CS_SUCCESS && !cancelled && done != all) {
            unsigned index = 0;
            for (; index < batch.capacity; ++index)
                if ((all & (u64{1} << index)) && !(started & (u64{1} << index)) &&
                    !(batch.entries[index].dependencies & ~done)) break;
            if (index == batch.capacity) {
                if (!running) { error = CS_INVALID_ARG; changed.notify_all(); return; }
                changed.wait(lock);
                continue;
            }
            started |= u64{1} << index;
            ++running;
            lock.unlock();
            u64 result = 0;
            auto status = apiCall([&] { return batch.entries[index].request->executeBatchEntry(cancelled, this, &result); });
            lock.lock();
            --running;
            done |= u64{1} << index;
            value = result;
            if (status != CS_SUCCESS && error == CS_SUCCESS) error = status;
            changed.notify_all();
        }
    }
public:
    BatchExecution(const Batch &batch, std::atomic_bool &cancelled) : batch(batch), cancelled(cancelled) {}
    CS_STATUS run(u64 *out)
    {
        Device *device = nullptr;
        for (unsigned i = 0; i < batch.capacity; ++i) if (batch.entries[i].request) {
            all |= u64{1} << i;
            device = batch.entries[i].request->device;
        }
        if (!device) return CS_INVALID_ARG;
        unsigned concurrency = batch.mode == CS_BATCH_SERIAL ? 1 : std::min(device->concurrency, MaxBatch);
        std::vector<std::thread> workers;
        workers.reserve(concurrency - 1);
        for (unsigned i = 1; i < concurrency; ++i) {
            try { workers.emplace_back([this] { worker(); }); }
            catch (...) { break; } // 资源不足时用已启动线程和当前线程继续执行。
        }
        worker();
        for (auto &thread : workers) thread.join();
        auto finish = device->ops().finishBatchExecution;
        if (finish) {
            auto status = apiCall([&] { finish(device->privateData, this, error == CS_SUCCESS && !cancelled); return CS_SUCCESS; });
            if (error == CS_SUCCESS) error = status;
        }
        if (out) *out = value;
        return cancelled ? CS_ERROR_IN_EXECUTION : error;
    }
};
} // namespace
namespace snia {
CS_STATUS runBatch(const Batch &batch, std::atomic_bool &cancelled, u64 *value)
{ return BatchExecution(batch, cancelled).run(value); }
} // namespace snia
CS_STATUS csAllocBatchRequest(CS_BATCH_MODE mode, int capacity, CS_BATCH_HANDLE *out)
{
    return apiCall([&] {
        if (!out || capacity <= 0 || capacity > static_cast<int>(MaxBatch)) return CS_INVALID_ARG;
        *out = nullptr;
        if (mode < CS_BATCH_SERIAL || mode > CS_BATCH_HYBRID) return CS_INVALID_OPTION;
        *out = publishHandle(std::make_shared<Batch>(mode, capacity));
        return CS_SUCCESS;
    });
}
CS_STATUS csFreeBatchRequest(CS_BATCH_HANDLE handle)
{
    return apiCall([&] {
        auto batch = acquireHandle<Batch>(handle);
        if (!batch) return CS_INVALID_HANDLE;
        CloseHandle closing(batch);
        if (!closing) return CS_HANDLE_IN_USE;
        closing.commit();
        return CS_SUCCESS;
    });
}
CS_STATUS csConfigureBatchEntry(CS_BATCH_HANDLE handle, CS_BATCH_CONFIG_TYPE action, const CsBatchRequest *request,
                                CS_BATCH_INDEX before, CS_BATCH_INDEX after, CS_BATCH_INDEX *current)
{
    return apiCall([&] {
        auto batch = acquireHandle<Batch>(handle);
        if (!batch) return CS_INVALID_HANDLE;
        std::shared_ptr<PreparedRequest> prepared;
        if (action == CS_BATCH_ADD || action == CS_BATCH_RECONFIG) {
            if (!request) return CS_INVALID_ARG;
            prepared = prepareRequest(*request);
        }
        std::lock_guard<std::mutex> lock(handlesMutex);
        if (batch.use_count() != 2) return CS_HANDLE_IN_USE;
        auto draft = batch->entries;
        if (before > batch->capacity || after > batch->capacity ||
            (before && !draft[before - 1].request) || (after && !draft[after - 1].request)) return CS_INVALID_ID;
        if (batch->mode == CS_BATCH_PARALLEL && ((action == CS_BATCH_ADD && (before || after)) ||
            action == CS_BATCH_JOIN || action == CS_BATCH_SPLIT)) return CS_INVALID_OPTION;
        u64 bitBefore = before ? u64{1} << (before - 1) : 0;
        u64 bitAfter = after ? u64{1} << (after - 1) : 0;
        unsigned index = 0;
        switch (action) {
        case CS_BATCH_ADD:
            if (!current) return CS_INVALID_ARG;
            while (index < batch->capacity && draft[index].request) ++index;
            if (index == batch->capacity) return CS_OUT_OF_RESOURCES;
            for (const auto &entry : draft)
                if (entry.request && entry.request->device != prepared->device) return CS_UNSUPPORTED;
            draft[index] = {prepared, bitAfter};
            if (before) draft[before - 1].dependencies = (draft[before - 1].dependencies & ~bitAfter) | (u64{1} << index);
            break;
        case CS_BATCH_JOIN:
            if (request || current || !before || !after) return CS_INVALID_ARG;
            draft[before - 1].dependencies |= bitAfter;
            break;
        case CS_BATCH_SPLIT:
            if (request || current || (!before && !after)) return CS_INVALID_ARG;
            if (before && after) draft[after - 1].dependencies &= ~bitBefore;
            else if (before) { for (auto &entry : draft) entry.dependencies &= ~bitBefore; }
            else draft[after - 1].dependencies = 0;
            break;
        case CS_BATCH_DELETE:
            if (request || current || !before || after) return CS_INVALID_ARG;
            draft[before - 1] = {};
            for (auto &entry : draft) entry.dependencies &= ~bitBefore;
            break;
        case CS_BATCH_RECONFIG:
            if (current || !before || after) return CS_INVALID_ARG;
            if (prepared->device != draft[before - 1].request->device) return CS_UNSUPPORTED;
            draft[before - 1].request = std::move(prepared);
            break;
        default: return CS_INVALID_OPTION;
        }
        if (!acyclic(draft, batch->capacity)) return CS_INVALID_ARG;
        batch->entries = std::move(draft);
        if (action == CS_BATCH_ADD) *current = index + 1;
        return CS_SUCCESS;
    });
}
CS_STATUS csHelperResizeBatchRequest(CS_BATCH_HANDLE handle, int capacity)
{
    return apiCall([&] {
        if (capacity <= 0 || capacity > static_cast<int>(MaxBatch)) return CS_INVALID_ARG;
        auto batch = acquireHandle<Batch>(handle);
        if (!batch) return CS_INVALID_HANDLE;
        std::lock_guard<std::mutex> lock(handlesMutex);
        if (batch.use_count() != 2) return CS_HANDLE_IN_USE;
        for (unsigned i = capacity; i < batch->capacity; ++i) if (batch->entries[i].request) return CS_INVALID_LENGTH;
        batch->capacity = capacity;
        return CS_SUCCESS;
    });
}
CS_STATUS csQueueBatchRequest(CS_BATCH_HANDLE handle, void *context, csQueueCallbackFn callback,
                              CS_EVT_HANDLE event, CS_REQ_HANDLE *out, u64 *value)
{
    return apiCall([&] {
        auto batch = acquireHandle<Batch>(handle);
        if (!batch) return CS_INVALID_HANDLE;
        return submitRequest({}, std::move(batch), context, callback, event, out, value);
    });
}
