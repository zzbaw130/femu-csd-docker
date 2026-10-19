/* Internal request dispatch shared by normal and spill-reentry execution. */
#include "femu_common.h"
/* FEMU 内部使用：仅重入路径额外传递 cparam2，不改变标准计算请求。 */
CS_STATUS cs_task_run(struct cs_task *t, u64 *value)
{
    if (value) *value = 0;
    switch (t->req->ReqType) {
    case CS_QUEUE_COMPUTE:
        return femu_compute(t->dev, &t->req->u.Compute, t->reentry, value);
    case CS_COPY_AFDM:
        return femu_queueCopyMemRequest(t->dev, &t->req->u.CopyMem, value);
    case CS_STORAGE_IO:
        return femu_queueStorageRequest(t->dev, &t->req->u.StorageIo, value);
    default:
        return CS_INVALID_OPTION;
    }
}
