/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * File intent:
 *   Most-recent-message service. It manages fixed buffer pools, publish/get
 *   sequencing and a bounded lease table so task cleanup can drop outstanding
 *   MRM references.
 *
 * Contracts/invariants:
 *   - currBufPtr is either NULL or a valid allocated RK_MRM_BUF.
 *   - nUsers counts outstanding kMRMGet() leases for that buffer.
 *   - Reserve grants an exclusive unpublished buffer to the current task.
 *   - Publish drops that reservation and becomes the new currBufPtr.
 *   - Runtime reserve/get/publish/unget are module-local because the API
 *     returns RK_MRM_BUF lease pointers.
 *   - Cleanup walks the bounded lease table so task death cannot leak buffers.
 */

#define RK_SOURCE_CODE

#include <kdynobjs.h>
#include <kmrm.h>
#include <ksch.h>
#include <ksyscall.h>
#include <ktrace.h>

#if (RK_CONF_MRM == ON)
struct RK_STRUCT_MRM_LEASE
{
    RK_MRM *mrmPtr;
    RK_MRM_BUF *bufPtr;
    USHORT getCount;
    RK_TID taskTid;
    BYTE reserved;
} K_ALIGN(4);

static struct RK_STRUCT_MRM_LEASE RK_gMrmLeasePool[RK_CONF_MRM_LEASES_MAX];

/* Validate the caller can read/write exactly one configured MRM payload. */
static RK_ERR kMRMUserPayloadReadValid_(RK_MRM const *const kobj,
                                        VOID const *const ptr)
{
    ULONG bytes;

    if ((kobj == NULL) || (ptr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->size > (RK_ULONG_MAX / (ULONG)RK_WORD_SIZE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    bytes = kobj->size * (ULONG)RK_WORD_SIZE;
    if ((RK_gSyscallThreadModeActive != 0U) &&
        (kMpuUserReadValid(RK_gRunPtr, ptr, bytes) != RK_TRUE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    return (RK_ERR_SUCCESS);
}

static RK_ERR kMRMUserPayloadWriteValid_(RK_MRM const *const kobj,
                                         VOID *const ptr)
{
    ULONG bytes;

    if ((kobj == NULL) || (ptr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->size > (RK_ULONG_MAX / (ULONG)RK_WORD_SIZE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    bytes = kobj->size * (ULONG)RK_WORD_SIZE;
    if ((RK_gSyscallThreadModeActive != 0U) &&
        (kMpuUserWriteValid(RK_gRunPtr, ptr, bytes) != RK_TRUE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    return (RK_ERR_SUCCESS);
}

/* Convert the public MRM handle into a checked kernel object pointer. */
static RK_ERR kMRMResolve_(RK_MRM_HANDLE const mrmHandle,
                           RK_MRM **const mrmPPtr)
{
    if (mrmPPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    *mrmPPtr = NULL;

    VOID *objPtr = NULL;
    RK_ERR const err =
        kDynObjResolveHandle(RK_DYN_OBJ_TYPE_MRM, mrmHandle, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
    *mrmPPtr = (RK_MRM *)objPtr;
    return (RK_ERR_SUCCESS);
}

static RK_ERR kMRMModuleLocalAccessErr_(RK_MRM const *const kobj)
{
    RK_MODULE const *const callerModulePtr =
        (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr : NULL;

    if (kobj == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (callerModulePtr == NULL)
    {
        return (RK_ERR_SUCCESS);
    }

    if (kobj->scope != RK_SCOPE_MODULE_LOCAL)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    return (kObjHeaderModuleLocalAccessErr(&kobj->header, callerModulePtr));
}

/******************************************************************************/
/* MRM Buffers                                                                */
/******************************************************************************/
/*
 * MRM uses two partitions: one for RK_MRM_BUF metadata and one for payload
 * storage. Both pieces must be allocated, still owned by their partitions and
 * not currently on a free list before a buffer can participate in publish/get.
 */
static RK_BOOL kMRMPartValid_(RK_MEM_PARTITION const *const partPtr)
{
    return (kObjHeaderReady((partPtr != NULL) ? &partPtr->header : NULL,
                            RK_MEMALLOC_KOBJ_ID));
}

static RK_BOOL kMRMPartOwnsBlock_(RK_MEM_PARTITION const *const partPtr,
                                  VOID const *const blockPtr,
                                  ULONG const minSize)
{
    if ((kMRMPartValid_(partPtr) == RK_FALSE) || (blockPtr == NULL) ||
        (partPtr->blkSize < minSize))
    {
        return (RK_FALSE);
    }

    BYTE const *const poolStartPtr = partPtr->poolPtr;
    BYTE const *const poolEndPtr =
        poolStartPtr + (partPtr->blkSize * partPtr->nMaxBlocks);
    BYTE const *const blockBytePtr = (BYTE const *)blockPtr;
    if ((blockBytePtr < poolStartPtr) || (blockBytePtr >= poolEndPtr))
    {
        return (RK_FALSE);
    }

    ULONG const diff = (ULONG)(blockBytePtr - poolStartPtr);
    return (((diff % partPtr->blkSize) == 0UL) ? RK_TRUE : RK_FALSE);
}

static RK_BOOL kMRMPartFreeListContains_(RK_MEM_PARTITION const *const partPtr,
                                         VOID const *const blockPtr)
{
    BYTE *freeBlockPtr = partPtr->freeListPtr;

    for (ULONG i = 0UL; (i < partPtr->nFreeBlocks) && (freeBlockPtr != NULL);
         i++)
    {
        if ((VOID const *)freeBlockPtr == blockPtr)
        {
            return (RK_TRUE);
        }

        freeBlockPtr = *(BYTE **)freeBlockPtr;
    }

    return (RK_FALSE);
}

static RK_BOOL kMRMPartOwnsAllocatedBlock_(RK_MEM_PARTITION const *const partPtr,
                                           VOID const *const blockPtr,
                                           ULONG const minSize)
{
    if (kMRMPartOwnsBlock_(partPtr, blockPtr, minSize) == RK_FALSE)
    {
        return (RK_FALSE);
    }

    return ((kMRMPartFreeListContains_(partPtr, blockPtr) == RK_FALSE)
                ? RK_TRUE
                : RK_FALSE);
}

static RK_BOOL kMRMBufferValid_(RK_MRM const *const kobj,
                                RK_MRM_BUF const *const bufPtr)
{
    if ((kobj == NULL) || (bufPtr == NULL) ||
        (kobj->size > (RK_ULONG_MAX / (ULONG)RK_WORD_SIZE)))
    {
        return (RK_FALSE);
    }

    ULONG const dataSizeBytes = kobj->size * (ULONG)RK_WORD_SIZE;

    if (kMRMPartOwnsAllocatedBlock_(&kobj->mrmMem, bufPtr,
                                    sizeof(RK_MRM_BUF)) == RK_FALSE)
    {
        return (RK_FALSE);
    }

    return (kMRMPartOwnsAllocatedBlock_(&kobj->mrmDataMem, bufPtr->mrmData,
                                        dataSizeBytes));
}

static RK_ERR kMRMBadPoolBlock_(VOID)
{
#if (RK_CONF_ERR_CHECK == ON)
    K_ERR_HANDLER(RK_FAULT_MEM_FREE);
#endif
    return (RK_ERR_MEM_FREE);
}

/* Return both metadata and payload blocks; callers have already validated them. */
static RK_ERR kMRMReleaseBuffer_(RK_MRM *const kobj,
                                 RK_MRM_BUF *const bufPtr)
{
    if ((kobj == NULL) || (bufPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    VOID *const mrmDataPtr = bufPtr->mrmData;
    RK_ERR err = kMemPartitionFree(&kobj->mrmDataMem, mrmDataPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    return (kMemPartitionFree(&kobj->mrmMem, (VOID *)bufPtr));
}

static RK_MRM_BUF *kMRMBufferAlloc_(RK_MRM *const kobj)
{
    RK_MRM_BUF *const allocPtr = kMemPartitionAlloc(&kobj->mrmMem);
    if (allocPtr == NULL)
    {
        return (NULL);
    }

    allocPtr->nUsers = 0UL;
    allocPtr->mrmData = (ULONG *)kMemPartitionAlloc(&kobj->mrmDataMem);
    if (allocPtr->mrmData == NULL)
    {
        allocPtr->mrmData = NULL;
        (VOID)kMemPartitionFree(&kobj->mrmMem, allocPtr);
        return (NULL);
    }

    return (allocPtr);
}

static RK_TCB *kMRMLeaseTask_(VOID)
{
    if ((RK_gRunPtr == NULL) || (RK_gRunPtr->init != RK_TRUE) ||
        (RK_gRunPtr->tid >= RK_NTHREADS))
    {
        return (NULL);
    }

    return (RK_gRunPtr);
}

/*
 * Lease entries are keyed by task, MRM and buffer. That lets one task hold
 * multiple get references to the same current value while cleanup can still
 * release exactly the references owned by a terminating task.
 */
static struct RK_STRUCT_MRM_LEASE *kMRMLeaseFind_(
    RK_TCB *const taskPtr,
    RK_MRM const *const mrmPtr,
    RK_MRM_BUF const *const bufPtr)
{
    if ((taskPtr == NULL) || (mrmPtr == NULL) || (bufPtr == NULL))
    {
        return (NULL);
    }

    for (UINT i = 0U; i < RK_CONF_MRM_LEASES_MAX; i++)
    {
        struct RK_STRUCT_MRM_LEASE *const leasePtr = &RK_gMrmLeasePool[i];
        if ((leasePtr->taskTid == taskPtr->tid) &&
            (leasePtr->mrmPtr == mrmPtr) &&
            (leasePtr->bufPtr == bufPtr))
        {
            return (leasePtr);
        }
    }

    return (NULL);
}

static struct RK_STRUCT_MRM_LEASE *kMRMLeaseFindFree_(RK_TCB *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return (NULL);
    }

    for (UINT i = 0U; i < RK_CONF_MRM_LEASES_MAX; i++)
    {
        struct RK_STRUCT_MRM_LEASE *const leasePtr = &RK_gMrmLeasePool[i];
        if (leasePtr->mrmPtr == NULL)
        {
            return (leasePtr);
        }
    }

    return (NULL);
}

/* A reserved buffer is exclusive until publish or cancellation drops it. */
static RK_BOOL kMRMLeaseReservedByOther_(RK_TCB const *const ownerPtr,
                                         RK_MRM const *const mrmPtr,
                                         RK_MRM_BUF const *const bufPtr)
{
    if ((mrmPtr == NULL) || (bufPtr == NULL))
    {
        return (RK_FALSE);
    }

    for (UINT i = 0U; i < RK_CONF_MRM_LEASES_MAX; i++)
    {
        struct RK_STRUCT_MRM_LEASE const *const leasePtr =
            &RK_gMrmLeasePool[i];
        if ((leasePtr->mrmPtr == NULL) ||
            (leasePtr->taskTid == ownerPtr->tid) ||
            (leasePtr->taskTid >= RK_NTHREADS))
        {
            continue;
        }

        RK_TCB const *const taskPtr = RK_gTaskHandleByPid[leasePtr->taskTid];
        if ((taskPtr != NULL) &&
            (taskPtr->init == RK_TRUE) &&
            (leasePtr->mrmPtr == mrmPtr) &&
            (leasePtr->bufPtr == bufPtr) &&
            (leasePtr->reserved == RK_TRUE))
        {
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static RK_BOOL kMRMBufferReserved_(RK_MRM const *const mrmPtr,
                                   RK_MRM_BUF const *const bufPtr)
{
    if ((mrmPtr == NULL) || (bufPtr == NULL))
    {
        return (RK_FALSE);
    }

    for (UINT i = 0U; i < RK_CONF_MRM_LEASES_MAX; i++)
    {
        struct RK_STRUCT_MRM_LEASE const *const leasePtr =
            &RK_gMrmLeasePool[i];
        if ((leasePtr->mrmPtr != mrmPtr) ||
            (leasePtr->bufPtr != bufPtr) ||
            (leasePtr->reserved != RK_TRUE) ||
            (leasePtr->taskTid >= RK_NTHREADS))
        {
            continue;
        }

        RK_TCB const *const taskPtr = RK_gTaskHandleByPid[leasePtr->taskTid];
        if ((taskPtr != NULL) && (taskPtr->init == RK_TRUE))
        {
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

RK_BOOL kMRMHasActiveLease(RK_MRM const *const mrmPtr)
{
    if (mrmPtr == NULL)
    {
        return (RK_FALSE);
    }

    for (UINT i = 0U; i < RK_CONF_MRM_LEASES_MAX; i++)
    {
        struct RK_STRUCT_MRM_LEASE const *const leasePtr =
            &RK_gMrmLeasePool[i];
        if ((leasePtr->mrmPtr != mrmPtr) ||
            (leasePtr->taskTid >= RK_NTHREADS))
        {
            continue;
        }

        RK_TCB const *const taskPtr = RK_gTaskHandleByPid[leasePtr->taskTid];
        if ((taskPtr != NULL) && (taskPtr->init == RK_TRUE) &&
            ((leasePtr->reserved == RK_TRUE) ||
             (leasePtr->getCount != 0U)))
        {
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

/* Empty lease records are returned to the bounded lease pool immediately. */
static VOID kMRMLeaseClearIfIdle_(struct RK_STRUCT_MRM_LEASE *const leasePtr)
{
    if ((leasePtr != NULL) && (leasePtr->getCount == 0UL) &&
        (leasePtr->reserved != RK_TRUE))
    {
        leasePtr->mrmPtr = NULL;
        leasePtr->bufPtr = NULL;
        leasePtr->taskTid = 0U;
    }
}

static RK_ERR kMRMLeaseReserveTrack_(RK_MRM *const mrmPtr,
                                     RK_MRM_BUF *const bufPtr)
{
    RK_TCB *const taskPtr = kMRMLeaseTask_();
    if (taskPtr == NULL)
    {
        return (RK_ERR_SUCCESS);
    }

    struct RK_STRUCT_MRM_LEASE *leasePtr =
        kMRMLeaseFind_(taskPtr, mrmPtr, bufPtr);
    if (leasePtr == NULL)
    {
        leasePtr = kMRMLeaseFindFree_(taskPtr);
    }

    if (leasePtr == NULL)
    {
        return (RK_ERR_BUFFER_FULL);
    }

    if (kMRMLeaseReservedByOther_(taskPtr, mrmPtr, bufPtr) == RK_TRUE)
    {
        return (RK_ERR_TASK_INVALID_ST);
    }

    if (leasePtr->reserved == RK_TRUE)
    {
        return (RK_ERR_TASK_INVALID_ST);
    }

    leasePtr->mrmPtr = mrmPtr;
    leasePtr->bufPtr = bufPtr;
    leasePtr->taskTid = taskPtr->tid;
    leasePtr->reserved = RK_TRUE;
    return (RK_ERR_SUCCESS);
}

static RK_ERR kMRMLeaseReserveDrop_(RK_MRM *const mrmPtr,
                                    RK_MRM_BUF *const bufPtr)
{
    RK_TCB *const taskPtr = kMRMLeaseTask_();
    if (taskPtr == NULL)
    {
        return (RK_ERR_SUCCESS);
    }

    struct RK_STRUCT_MRM_LEASE *const leasePtr =
        kMRMLeaseFind_(taskPtr, mrmPtr, bufPtr);
    if ((leasePtr == NULL) || (leasePtr->reserved != RK_TRUE))
    {
        return (RK_ERR_MEM_FREE);
    }

    leasePtr->reserved = RK_FALSE;
    kMRMLeaseClearIfIdle_(leasePtr);
    return (RK_ERR_SUCCESS);
}

static RK_ERR kMRMLeaseGetTrack_(RK_MRM *const mrmPtr,
                                 RK_MRM_BUF *const bufPtr)
{
    RK_TCB *const taskPtr = kMRMLeaseTask_();
    if (taskPtr == NULL)
    {
        return (RK_ERR_SUCCESS);
    }

    struct RK_STRUCT_MRM_LEASE *leasePtr =
        kMRMLeaseFind_(taskPtr, mrmPtr, bufPtr);
    if (leasePtr == NULL)
    {
        leasePtr = kMRMLeaseFindFree_(taskPtr);
    }

    if (leasePtr == NULL)
    {
        return (RK_ERR_BUFFER_FULL);
    }

    if (leasePtr->getCount == (USHORT)0xFFFFU)
    {
        return (RK_ERR_BUFFER_FULL);
    }

    leasePtr->mrmPtr = mrmPtr;
    leasePtr->bufPtr = bufPtr;
    leasePtr->taskTid = taskPtr->tid;
    leasePtr->getCount++;
    return (RK_ERR_SUCCESS);
}

static RK_ERR kMRMLeaseGetDrop_(RK_MRM *const mrmPtr,
                                RK_MRM_BUF *const bufPtr)
{
    RK_TCB *const taskPtr = kMRMLeaseTask_();
    if (taskPtr == NULL)
    {
        return (RK_ERR_SUCCESS);
    }

    struct RK_STRUCT_MRM_LEASE *const leasePtr =
        kMRMLeaseFind_(taskPtr, mrmPtr, bufPtr);
    if ((leasePtr == NULL) || (leasePtr->getCount == 0UL))
    {
        return (RK_ERR_MEM_FREE);
    }

    leasePtr->getCount--;
    kMRMLeaseClearIfIdle_(leasePtr);
    return (RK_ERR_SUCCESS);
}

/*
 * Drop get references from a buffer and release it only when it is no longer
 * the current value. The current buffer remains available to future readers
 * even when its user count reaches zero.
 */
static RK_ERR kMRMBufferGetRelease_(RK_MRM *const kobj,
                                    RK_MRM_BUF *const bufPtr,
                                    ULONG const count,
                                    RK_BOOL *const releasedPtr)
{
    RK_ERR err = RK_ERR_SUCCESS;

    if (releasedPtr != NULL)
    {
        *releasedPtr = RK_FALSE;
    }

    if ((kobj == NULL) || (bufPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (count > bufPtr->nUsers)
    {
        return (RK_ERR_MEM_FREE);
    }

    bufPtr->nUsers -= count;
    if ((bufPtr->nUsers == 0UL) && (kobj->currBufPtr != bufPtr))
    {
        if (releasedPtr != NULL)
        {
            *releasedPtr = RK_TRUE;
        }
        err = kMRMReleaseBuffer_(kobj, bufPtr);
    }

    return (err);
}

RK_ERR kMRMTaskCleanup(RK_TCB *const taskPtr)
{
    RK_ERR err = RK_ERR_SUCCESS;

    if (taskPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    for (UINT i = 0U; i < RK_CONF_MRM_LEASES_MAX; i++)
    {
        struct RK_STRUCT_MRM_LEASE *const leasePtr = &RK_gMrmLeasePool[i];
        RK_MRM *const mrmPtr = leasePtr->mrmPtr;
        RK_MRM_BUF *const bufPtr = leasePtr->bufPtr;
        RK_BOOL released = RK_FALSE;

        if ((leasePtr->mrmPtr == NULL) ||
            (leasePtr->taskTid != taskPtr->tid) ||
            (mrmPtr == NULL) || (bufPtr == NULL))
        {
            continue;
        }

        if ((mrmPtr->init != RK_TRUE) ||
            (mrmPtr->objID != RK_MRM_KOBJ_ID) ||
            (kMRMBufferValid_(mrmPtr, bufPtr) == RK_FALSE))
        {
            err = RK_ERR_INVALID_OBJ;
        }
        else
        {
            if (leasePtr->getCount != 0UL)
            {
                RK_ERR const getErr =
                    kMRMBufferGetRelease_(mrmPtr, bufPtr,
                                          leasePtr->getCount, &released);
                if (getErr != RK_ERR_SUCCESS)
                {
                    err = getErr;
                }
            }

            if ((leasePtr->reserved == RK_TRUE) &&
                (leasePtr->getCount == 0UL) &&
                (released == RK_FALSE) &&
                (mrmPtr->currBufPtr != bufPtr))
            {
                RK_ERR const reserveErr = kMRMReleaseBuffer_(mrmPtr, bufPtr);
                if (reserveErr != RK_ERR_SUCCESS)
                {
                    err = reserveErr;
                }
            }
        }

        leasePtr->mrmPtr = NULL;
        leasePtr->bufPtr = NULL;
        leasePtr->getCount = 0UL;
        leasePtr->taskTid = 0U;
        leasePtr->reserved = RK_FALSE;
    }

    return (err);
}

RK_ERR kMRMInit(RK_MRM *const kobj, RK_MRM_BUF *const mrmPoolPtr,
                VOID *mesgPoolPtr, ULONG const nBufs, ULONG const dataSizeWords)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        RK_MRM_INIT_SYSCALL_ARGS syscallArgs;

        syscallArgs.mrmPtr = kobj;
        syscallArgs.mrmPoolPtr = mrmPoolPtr;
        syscallArgs.mesgPoolPtr = mesgPoolPtr;
        syscallArgs.nBufs = nBufs;
        syscallArgs.dataSizeWords = dataSizeWords;

        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MRM_INIT,
                                        (ULONG)(UINTPTR)&syscallArgs,
                                        0UL, 0UL, 0UL));
    }

    RK_ERR const phaseErr = kKernelRawInitGuard();
    if (phaseErr != RK_ERR_SUCCESS)
    {
        return (phaseErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)

    if (kobj == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->init == RK_TRUE)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_DOUBLE_INIT);
        RK_CR_EXIT
        return (RK_ERR_OBJ_DOUBLE_INIT);
    }

    if (mrmPoolPtr == NULL || mesgPoolPtr == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

#endif
    if ((nBufs == 0UL) || (dataSizeWords == 0UL) ||
        (dataSizeWords > (RK_ULONG_MAX / (ULONG)RK_WORD_SIZE)))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    RK_ERR err = RK_ERR_ERROR;
    RK_BOOL mrmMemInit = RK_FALSE;

    err =
        kMemPartitionInit(&kobj->mrmMem, mrmPoolPtr, sizeof(RK_MRM_BUF), nBufs);
    if (err == RK_ERR_SUCCESS)
    {
        mrmMemInit = RK_TRUE;
        err = kMemPartitionInit(&kobj->mrmDataMem, mesgPoolPtr,
                                dataSizeWords * (ULONG)RK_WORD_SIZE, nBufs);
    }
    if (err == RK_ERR_SUCCESS)
    {
        /* nobody is using anything yet */
        kobj->currBufPtr = NULL;
        kobj->init = RK_TRUE;
        kobj->size = dataSizeWords;
        kobj->objID = RK_MRM_KOBJ_ID;
        kobj->objName[0] = '\0';
        kObjHeaderOwnerModuleSet(&kobj->header,
                                 (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr
                                                      : NULL);
        kTraceRegisterObject(kobj, RK_MRM_KOBJ_ID);
    }
    else if (mrmMemInit == RK_TRUE)
    {
        kTraceUnregisterObject(&kobj->mrmMem);
        RK_MEMSET(&kobj->mrmMem, 0, sizeof(kobj->mrmMem));
    }

    RK_CR_EXIT
    return (err);
}

RK_MRM_BUF *kMRMReserve(RK_MRM_HANDLE const mrmHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_MRM_BUF *)(UINTPTR)kSyscallInvoke4(
            RK_SYSCALL_MRM_RESERVE, (ULONG)(UINTPTR)mrmHandle, 0UL, 0UL,
            0UL));
    }

    RK_MRM *kobj = NULL;
    RK_ERR const resolveErr = kMRMResolve_(mrmHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (NULL);
    }

    RK_CR_AREA
    RK_CR_ENTER

    if (kobj == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (NULL);
    }
    if (kobj->init != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
#endif
        RK_CR_EXIT
        return (NULL);
    }

    if (kobj->objID != RK_MRM_KOBJ_ID)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (NULL);
    }

    RK_ERR const accessErr = kMRMModuleLocalAccessErr_(kobj);
    if (accessErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (NULL);
    }

    RK_MRM_BUF *allocPtr = NULL;
    if ((kobj->currBufPtr != NULL) &&
        (kobj->currBufPtr->nUsers == 0UL) &&
        (kMRMBufferReserved_(kobj, kobj->currBufPtr) != RK_TRUE))
    {
        allocPtr = kobj->currBufPtr;
        allocPtr->nUsers = 0UL;
    }
    else
    {
        allocPtr = kMRMBufferAlloc_(kobj);
    }
    if (allocPtr != NULL)
    {
        RK_ERR const leaseErr = kMRMLeaseReserveTrack_(kobj, allocPtr);
        if (leaseErr != RK_ERR_SUCCESS)
        {
            if (allocPtr != kobj->currBufPtr)
            {
                (VOID)kMRMReleaseBuffer_(kobj, allocPtr);
            }
            allocPtr = NULL;
        }
    }
    kTraceRecordObject(kobj, RK_TRACE_OP_RESERVE,
                       (allocPtr != NULL) ? RK_ERR_SUCCESS : RK_ERR_BUFFER_EMPTY,
                       kobj->mrmMem.nFreeBlocks);
    RK_CR_EXIT
    return (allocPtr);
}

RK_ERR kMRMPublish(RK_MRM_HANDLE const mrmHandle, RK_MRM_BUF *const bufPtr,
                   VOID const *pubMesgPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MRM_PUBLISH,
                                        (ULONG)(UINTPTR)mrmHandle,
                                        (ULONG)(UINTPTR)bufPtr,
                                        (ULONG)(UINTPTR)pubMesgPtr, 0UL));
    }

    RK_MRM *kobj = NULL;
    RK_ERR const resolveErr = kMRMResolve_(mrmHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    if (kobj == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }
    if (!kobj->init)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (kobj->objID != RK_MRM_KOBJ_ID)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    RK_ERR const accessErr = kMRMModuleLocalAccessErr_(kobj);
    if (accessErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (accessErr);
    }

    if (bufPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }
    if (pubMesgPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }
    if ((kMRMBufferValid_(kobj, bufPtr) == RK_FALSE) ||
        (bufPtr->nUsers != 0UL))
    {
        RK_CR_EXIT
        return (kMRMBadPoolBlock_());
    }

    RK_ERR const userErr = kMRMUserPayloadReadValid_(kobj, pubMesgPtr);
    if (userErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (userErr);
    }

    RK_ERR const leaseErr = kMRMLeaseReserveDrop_(kobj, bufPtr);
    if (leaseErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (leaseErr);
    }

    if ((kobj->currBufPtr != NULL) && (kobj->currBufPtr != bufPtr) &&
        (kobj->currBufPtr->nUsers == 0UL) &&
        (kMRMBufferReserved_(kobj, kobj->currBufPtr) != RK_TRUE))
    {
        RK_ERR err = kMRMReleaseBuffer_(kobj, kobj->currBufPtr);
        if (err != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (err);
        }
    }

    ULONG *mrmMesgPtr_ = (ULONG *)bufPtr->mrmData;
    const ULONG *pubMesgPtr_ = (const ULONG *)pubMesgPtr;
    for (UINT i = 0; i < kobj->size; ++i)
    {
        mrmMesgPtr_[i] = pubMesgPtr_[i];
    }
    kobj->currBufPtr = bufPtr;
    kTraceRecordObject(kobj, RK_TRACE_OP_PUBLISH, RK_ERR_SUCCESS,
                       bufPtr->nUsers);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_MRM_BUF *kMRMGet(RK_MRM_HANDLE const mrmHandle, VOID *const getMesgPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_MRM_BUF *)(UINTPTR)kSyscallInvoke4(
            RK_SYSCALL_MRM_GET, (ULONG)(UINTPTR)mrmHandle,
            (ULONG)(UINTPTR)getMesgPtr, 0UL, 0UL));
    }

    RK_MRM *kobj = NULL;
    RK_ERR const resolveErr = kMRMResolve_(mrmHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (NULL);
    }

    RK_CR_AREA
    RK_CR_ENTER

    if (kobj == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (NULL);
    }

    if (kobj->init != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
#endif
        RK_CR_EXIT
        return (NULL);
    }

    if (kobj->objID != RK_MRM_KOBJ_ID)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (NULL);
    }

    RK_ERR const accessErr = kMRMModuleLocalAccessErr_(kobj);
    if (accessErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (NULL);
    }

    if (getMesgPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (NULL);
    }

    if ((kobj->currBufPtr == NULL) ||
        (kMRMBufferReserved_(kobj, kobj->currBufPtr) == RK_TRUE))
    {
        RK_CR_EXIT
        return (NULL);
    }

    if (kMRMUserPayloadWriteValid_(kobj, getMesgPtr) != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (NULL);
    }

    RK_ERR const leaseErr = kMRMLeaseGetTrack_(kobj, kobj->currBufPtr);
    if (leaseErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (NULL);
    }

    kobj->currBufPtr->nUsers++;
    ULONG *getMesgPtr_ = (ULONG *)getMesgPtr;
    ULONG const *mrmMesgPtr_ = (ULONG const *)kobj->currBufPtr->mrmData;
    for (ULONG i = 0; i < kobj->size; ++i)
    {
        getMesgPtr_[i] = mrmMesgPtr_[i];
    }
    kTraceRecordObject(kobj, RK_TRACE_OP_GET, RK_ERR_SUCCESS,
                       kobj->currBufPtr->nUsers);
    RK_CR_EXIT
    return (kobj->currBufPtr);
}

RK_ERR kMRMUnget(RK_MRM_HANDLE const mrmHandle, RK_MRM_BUF *const bufPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MRM_UNGET,
                                        (ULONG)(UINTPTR)mrmHandle,
                                        (ULONG)(UINTPTR)bufPtr,
                                        0UL, 0UL));
    }

    RK_MRM *kobj = NULL;
    RK_ERR const resolveErr = kMRMResolve_(mrmHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    if (kobj == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (!kobj->init)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (bufPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->objID != RK_MRM_KOBJ_ID)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    RK_ERR const accessErr = kMRMModuleLocalAccessErr_(kobj);
    if (accessErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (accessErr);
    }

    if (kMRMBufferValid_(kobj, bufPtr) == RK_FALSE)
    {
        RK_CR_EXIT
        return (kMRMBadPoolBlock_());
    }

    if (bufPtr->nUsers == 0UL)
    {
        RK_CR_EXIT
        return (kMRMBadPoolBlock_());
    }

    RK_ERR err = RK_ERR_SUCCESS;
    ULONG const remainingUsers = bufPtr->nUsers - 1UL;
    err = kMRMLeaseGetDrop_(kobj, bufPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kMRMBufferGetRelease_(kobj, bufPtr, 1UL, NULL);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    kTraceRecordObject(kobj, RK_TRACE_OP_UNGET, err, remainingUsers);
    RK_CR_EXIT
    return (err);
}
#endif
