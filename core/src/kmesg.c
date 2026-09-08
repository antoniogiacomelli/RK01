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
 *   Asynchronous message primitives. The legacy direct path transfers
 *   application-owned RK_MESG buffers by reference between same-domain
 *   endpoints. The copy path transfers task-addressed payloads by value through
 *   a bounded kernel-owned RK_MESG pool, which is safe across MPU domain
 *   boundaries.
 *
 * Contracts/invariants:
 *   - A message buffer is either free, allocated, queued or received.
 *   - A non-free message has one logical owner for cleanup and priority
 *     ceiling accounting.
 *   - Direct by-reference transfer requires same-domain endpoints under MPU.
 *   - Copy-message RK_MESG blocks are freed by the kernel after delivery or
 *     cleanup.
 *   - A blocked copy receiver is woken only if the payload fits its advertised
 *     receive buffer; oversized messages remain queued.
 *   - Payload size is validated and word-aligned before pool allocation.
 */

/*
 * The by-reference asynchronous direct path uses RK_MESG buffers from caller
 * pools. Under MPU, this pointer-transfer primitive is restricted to tasks in
 * the same domain: inter-domain payload exchange must use message queues or
 * task-addressed copy messages.
 *
 * Ceiling protocol:
 * - kMesgPoolInit() may attach a priority ceiling to a message pool.
 * - RK_MESG_PRIO_CEILING_NONE disables the protocol for that pool.
 * - While a task owns at least one message from a ceiling-enabled pool, the
 *   scheduler raises that task's effective priority to at least the pool
 *   ceiling.
 * - Ownership is tracked by each task's asynchMesgOwnedList. kMesgSetOwner_()
 *   is the transfer point that updates this list and triggers effective
 *   priority recomputation for the old and new owners.
 * - Ownership starts when a task allocates a message, transfers to the receiver
 *   at send time, stays with the receiver while queued/received, and ends when
 *   the message is freed or handed directly to a blocked allocator.
 */



#define RK_SOURCE_CODE

#include <kmesg.h>
#include <kmem.h>
#include <ksch.h>
#include <ksyscall.h>
#include <ktimer.h>
#include <ktrace.h>
#include <kerr.h>
#include <kstring.h>

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))


#ifndef K_GET_MESG_ADDR
#define K_GET_MESG_ADDR(nodePtr) K_GET_CONTAINER_ADDR(nodePtr, RK_MESG, mesgNode)
#endif

/*
 * Return the total partition-block size for a direct-message payload. The
 * result includes the RK_MESG header, rejects arithmetic overflow and rounds up
 * to the kernel word size used by the memory partition backing the pool.
 */
static inline ULONG kMesgBlockBytes_(ULONG const payloadBytes)
{
    if ((payloadBytes == 0UL) ||
        (payloadBytes > (RK_ULONG_MAX - sizeof(RK_MESG))))
    {
        return (0UL);
    }

    ULONG const mesgBytes = sizeof(RK_MESG) + payloadBytes;

    if (mesgBytes > (RK_ULONG_MAX - (RK_WORD_SIZE - 1UL)))
    {
        return (0UL);
    }

    return ((mesgBytes + RK_WORD_SIZE - 1UL) &
            ~(RK_WORD_SIZE - 1UL));
}

static RK_ERR kMesgUserBlockWriteValid_(RK_TCB const *const taskPtr,
                                        RK_MESG const *const mesgPtr)
{
    ULONG blockBytes;

    if (mesgPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    blockBytes = kMesgBlockBytes_(mesgPtr->payloadBytes);
    if (blockBytes == 0UL)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    return ((kMpuUserWriteValid(taskPtr, (VOID *)mesgPtr, blockBytes) ==
             RK_TRUE)
                ? RK_ERR_SUCCESS
                : RK_ERR_INVALID_PARAM);
}

RK_FORCE_INLINE
static inline RK_BOOL kMesgIsValid_(RK_MESG const *const mesgPtr)
{
    return (((mesgPtr != NULL) && (mesgPtr->objID == RK_MESG_KOBJ_ID))
                ? RK_TRUE
                : RK_FALSE);
}


RK_FORCE_INLINE
static inline RK_BOOL kMesgStateOwned_(RK_MESG const *const mesgPtr)
{
    return (((mesgPtr->state == RK_MESG_STATE_ALLOCATED) ||
             (mesgPtr->state == RK_MESG_STATE_RECEIVED))
                ? RK_TRUE
                : RK_FALSE);
}
RK_FORCE_INLINE
static inline RK_BOOL kMesgStateHasSender_(RK_MESG const *const mesgPtr)
{
    return (((mesgPtr->state == RK_MESG_STATE_QUEUED) ||
             (mesgPtr->state == RK_MESG_STATE_RECEIVED))
                ? RK_TRUE
                : RK_FALSE);
}

static inline RK_BOOL kMesgTasksShareDomain_(RK_TCB const *const aPtr,
                                             RK_TCB const *const bPtr)
{
    return (((aPtr != NULL) && (bPtr != NULL) &&
             (aPtr->domainPtr == bPtr->domainPtr))
                ? RK_TRUE
                : RK_FALSE);
}

/* Translate internal reschedule status into the public success contract. */
RK_FORCE_INLINE
static inline RK_ERR kMesgPublicReadyErr_(RK_ERR const err)
{
    if ((err == RK_ERR_RESCHED_PENDING) ||
        (err == RK_ERR_RESCHED_NOT_NEEDED))
    {
        return (RK_ERR_SUCCESS);
    }

    return (err);
}

RK_FORCE_INLINE
static inline VOID kMesgClearWait_(RK_TCB *const taskPtr)
{
    taskPtr->asynchMesgWaitSenderPtr = NULL;
    taskPtr->asynchMesgWaitDestPtr = NULL;
    taskPtr->asynchMesgWaitStatus = RK_ERR_SUCCESS;
}

RK_FORCE_INLINE
static inline VOID kMesgClearAllocWait_(RK_TCB *const taskPtr)
{
    taskPtr->asynchMesgAllocDestPtr = NULL;
    taskPtr->timeoutNode.waitingQueuePtr = NULL;
}

#if (RK_CONF_ASYNCH_COPY_MESG == ON)
#define RK_MESG_COPY_BLOCK_BYTES_                                             \
    ((ULONG)((sizeof(RK_MESG) + RK_CONF_ASYNCH_COPY_MESG_PAYLOAD_BYTES +     \
              RK_WORD_SIZE - 1UL) &                                           \
             ~(RK_WORD_SIZE - 1UL)))
#define RK_MESG_COPY_BLOCK_WORDS_                                             \
    (RK_MESG_COPY_BLOCK_BYTES_ / RK_WORD_SIZE)

static ULONG copyMesgPool_[RK_CONF_ASYNCH_COPY_MESG_MAX]
                          [RK_MESG_COPY_BLOCK_WORDS_] K_ALIGN(4);
static RK_LIST copyMesgFreeList_;
static RK_BOOL copyMesgPoolInit_;

static inline VOID *kMesgPayloadRaw_(RK_MESG *const mesgPtr)
{
    return ((VOID *)((BYTE *)mesgPtr + sizeof(RK_MESG)));
}

static inline VOID const *kMesgPayloadRawConst_(RK_MESG const *const mesgPtr)
{
    return ((VOID const *)((BYTE const *)mesgPtr + sizeof(RK_MESG)));
}

static VOID kMesgCopyClearWait_(RK_TCB *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return;
    }

    taskPtr->asynchCopyMesgWaitSenderPtr = NULL;
    taskPtr->asynchCopyMesgRecvBufPtr = NULL;
    taskPtr->asynchCopyMesgRecvBufBytes = 0UL;
    taskPtr->asynchCopyMesgRecvBytesPtr = NULL;
    taskPtr->asynchCopyMesgRecvStatus = RK_ERR_SUCCESS;
    taskPtr->timeoutNode.waitingQueuePtr = NULL;
}

static RK_ERR kMesgCopyPoolEnsureInit_(VOID)
{
    if (copyMesgPoolInit_ == RK_TRUE)
    {
        return (RK_ERR_SUCCESS);
    }

    RK_ERR err = kListInit(&copyMesgFreeList_);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    for (ULONG idx = 0UL; idx < RK_CONF_ASYNCH_COPY_MESG_MAX; idx++)
    {
        RK_MESG *const mesgPtr =
            (RK_MESG *)(VOID *)&copyMesgPool_[idx][0];

        RK_MEMSET(mesgPtr, 0, RK_MESG_COPY_BLOCK_BYTES_);
        mesgPtr->state = RK_MESG_STATE_FREE;
        mesgPtr->objID = RK_INVALID_KOBJ;
        err = kListAddTail(&copyMesgFreeList_, &mesgPtr->mesgNode);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
    }

    copyMesgPoolInit_ = RK_TRUE;
    return (RK_ERR_SUCCESS);
}

static RK_MESG *kMesgCopyAlloc_(VOID)
{
    if (kMesgCopyPoolEnsureInit_() != RK_ERR_SUCCESS)
    {
        return (NULL);
    }

    if (copyMesgFreeList_.size == 0UL)
    {
        return (NULL);
    }

    RK_NODE *nodePtr = NULL;
    RK_ERR const err = kListRemoveHead(&copyMesgFreeList_, &nodePtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
    if ((err != RK_ERR_SUCCESS) || (nodePtr == NULL))
    {
        return (NULL);
    }

    RK_MESG *const mesgPtr = K_GET_MESG_ADDR(nodePtr);
    RK_MEMSET(mesgPtr, 0, RK_MESG_COPY_BLOCK_BYTES_);
    mesgPtr->poolPtr = NULL;
    mesgPtr->ownerPtr = NULL;
    mesgPtr->state = RK_MESG_STATE_ALLOCATED;
    mesgPtr->objID = RK_MESG_KOBJ_ID;
    return (mesgPtr);
}

static VOID kMesgCopyFree_(RK_MESG *const mesgPtr)
{
    if (mesgPtr == NULL)
    {
        return;
    }

    RK_MEMSET(mesgPtr, 0, RK_MESG_COPY_BLOCK_BYTES_);
    mesgPtr->state = RK_MESG_STATE_FREE;
    mesgPtr->objID = RK_INVALID_KOBJ;
    kListAddTail(&copyMesgFreeList_, &mesgPtr->mesgNode);
}

static inline RK_BOOL kMesgCopyWaitSenderMatches_(
    RK_TCB const *const receiverPtr,
    RK_TCB const *const senderPtr)
{
    if (receiverPtr->asynchCopyMesgWaitSenderPtr == NULL)
    {
        return (RK_TRUE);
    }

    return ((receiverPtr->asynchCopyMesgWaitSenderPtr == senderPtr) ? RK_TRUE
                                                                    : RK_FALSE);
}

static inline RK_BOOL kMesgCopyReceiverCanAccept_(
    RK_TCB const *const receiverPtr,
    RK_TCB const *const senderPtr,
    ULONG const bytes)
{
    if ((receiverPtr == NULL) ||
        (receiverPtr->asynchCopyMesgRecvBufPtr == NULL) ||
        (receiverPtr->asynchCopyMesgWaiters.size == 0UL))
    {
        return (RK_FALSE);
    }

    if (kMesgCopyWaitSenderMatches_(receiverPtr, senderPtr) != RK_TRUE)
    {
        return (RK_FALSE);
    }

    /*
     * Buffer fit is part of wake eligibility. A too-large pending message must
     * not wake the receiver because the receiver has not provided storage that
     * can complete the copy.
     */
    return ((bytes <= receiverPtr->asynchCopyMesgRecvBufBytes) ? RK_TRUE
                                                               : RK_FALSE);
}

static inline RK_BOOL kMesgTaskUnprivileged_(RK_TCB const *const taskPtr)
{
    return (((taskPtr != NULL) && ((taskPtr->savedControl & 0x1UL) != 0UL))
                ? RK_TRUE
                : RK_FALSE);
}

static RK_ERR kMesgCopyDisarmBlockingTimeout_(RK_TCB *const taskPtr)
{
    if (taskPtr->timeoutNode.timeoutType != RK_TIMEOUT_BLOCKING)
    {
        taskPtr->timeoutNode.waitingQueuePtr = NULL;
        return (RK_ERR_SUCCESS);
    }

    return (kTimeoutNodeDisarm(&taskPtr->timeoutNode));
}

static RK_BOOL kMesgCopyDeliverPayloadToWaiter_(
    RK_TCB *const receiverPtr,
    RK_TCB const *const senderPtr,
    VOID const *const payloadPtr,
    ULONG const bytes,
    RK_ERR *const errPtr)
{
    if (kMesgCopyReceiverCanAccept_(receiverPtr, senderPtr, bytes) != RK_TRUE)
    {
        return (RK_FALSE);
    }

    RK_TCB *waiterPtr = kTCBQPeek(&receiverPtr->asynchCopyMesgWaiters);
    K_ASSERT(waiterPtr == receiverPtr);
    RK_ERR err = kTCBQDeq(&receiverPtr->asynchCopyMesgWaiters, &waiterPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
    if (err != RK_ERR_SUCCESS)
    {
        *errPtr = err;
        return (RK_TRUE);
    }

    err = kMesgCopyDisarmBlockingTimeout_(receiverPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
    if (err != RK_ERR_SUCCESS)
    {
        *errPtr = err;
        return (RK_TRUE);
    }

    if ((RK_gSyscallThreadModeActive != 0U) &&
        (kMesgTaskUnprivileged_(receiverPtr) == RK_TRUE) &&
        (kMpuUserWriteValid(receiverPtr,
                            receiverPtr->asynchCopyMesgRecvBufPtr,
                            bytes) != RK_TRUE))
    {
        kMesgCopyClearWait_(receiverPtr);
        receiverPtr->asynchCopyMesgRecvStatus = RK_ERR_INVALID_PARAM;
        *errPtr = kMesgPublicReadyErr_(kReadySwtch(receiverPtr));
        return (RK_TRUE);
    }

    RK_MEMCPY(receiverPtr->asynchCopyMesgRecvBufPtr, payloadPtr, bytes);
    if (receiverPtr->asynchCopyMesgRecvBytesPtr != NULL)
    {
        *(receiverPtr->asynchCopyMesgRecvBytesPtr) = bytes;
    }
    kMesgCopyClearWait_(receiverPtr);
    receiverPtr->asynchCopyMesgRecvStatus = RK_ERR_SUCCESS;
    *errPtr = kMesgPublicReadyErr_(kReadySwtch(receiverPtr));
    return (RK_TRUE);
}

static RK_MESG *kMesgCopyDequeueMatching_(RK_TCB *const receiverPtr,
                                          RK_TCB const *const fromTaskPtr,
                                          ULONG const recvBytes)
{
    RK_NODE *nodePtr = receiverPtr->asynchCopyMesgQueue.listDummy.nextPtr;

    while (nodePtr != &receiverPtr->asynchCopyMesgQueue.listDummy)
    {
        RK_NODE *const nextPtr = nodePtr->nextPtr;
        RK_MESG *const mesgPtr = K_GET_MESG_ADDR(nodePtr);

        RK_BOOL const senderMatches =
            ((fromTaskPtr == NULL) ||
             ((mesgPtr->sender == kTaskHandleFromTcb(fromTaskPtr)) &&
              (mesgPtr->senderPid == fromTaskPtr->tid)))
                ? RK_TRUE
                : RK_FALSE;

        if ((senderMatches == RK_TRUE) &&
            (mesgPtr->payloadBytes <= recvBytes))
        {
            RK_ERR const err =
                kListRemove(&receiverPtr->asynchCopyMesgQueue, nodePtr);
            K_ASSERT(err == RK_ERR_SUCCESS);
            mesgPtr->state = RK_MESG_STATE_RECEIVED;
            return (mesgPtr);
        }

        nodePtr = nextPtr;
        RK_BARRIER
    }

    return (NULL);
}

static RK_ERR kMesgCopyStore_(RK_TCB *const receiverPtr,
                              RK_TCB *const senderPtr,
                              RK_TASK_HANDLE const taskHandle,
                              VOID const *const sendPtr,
                              ULONG const bytes)
{
    RK_MESG *const mesgPtr = kMesgCopyAlloc_();
    if (mesgPtr == NULL)
    {
        return (RK_ERR_BUFFER_EMPTY);
    }

    mesgPtr->sender = kTaskHandleFromTcb(senderPtr);
    mesgPtr->receiver = taskHandle;
    mesgPtr->senderPid = senderPtr->tid;
    mesgPtr->receiverPid = receiverPtr->tid;
    mesgPtr->payloadBytes = bytes;
    mesgPtr->state = RK_MESG_STATE_QUEUED;
    RK_MEMCPY(kMesgPayloadRaw_(mesgPtr), sendPtr, bytes);

    RK_ERR const err =
        kListAddTail(&receiverPtr->asynchCopyMesgQueue, &mesgPtr->mesgNode);
    if (err != RK_ERR_SUCCESS)
    {
        kMesgCopyFree_(mesgPtr);
    }
    return (err);
}

static VOID kMesgCopyReadyWithStatus_(RK_TCB *const taskPtr,
                                      RK_ERR const status)
{
    if (taskPtr == NULL)
    {
        return;
    }

    if (taskPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
    {
        kTimeoutNodeDisarm(&taskPtr->timeoutNode);
    }
    else
    {
        kTimeoutNodeReset(&taskPtr->timeoutNode);
    }

    kMesgCopyClearWait_(taskPtr);
    taskPtr->asynchCopyMesgRecvStatus = status;
    taskPtr->timeOut = RK_FALSE;
    kReadySwtch(taskPtr);
}

static VOID kMesgCopyCancelWaitersForSender_(RK_TCB const *const senderPtr)
{
    if (senderPtr == NULL)
    {
        return;
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_TCB *const receiverPtr = RK_gTaskHandleByPid[idx];
        if ((receiverPtr == NULL) ||
            (receiverPtr == senderPtr) ||
            (receiverPtr->init != RK_TRUE) ||
            (receiverPtr->asynchCopyMesgWaitSenderPtr != senderPtr) ||
            (receiverPtr->asynchCopyMesgWaiters.size == 0UL))
        {
            continue;
        }

        RK_TCB *waiterPtr = receiverPtr;
        RK_ERR const err =
            kTCBQRem(&receiverPtr->asynchCopyMesgWaiters, &waiterPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if (err == RK_ERR_SUCCESS)
        {
            kMesgCopyReadyWithStatus_(receiverPtr, RK_ERR_OBJ_NOT_INIT);
        }
    }
}
#endif


/*
 * Ownership is the priority-ceiling hook. This is the only helper that moves a
 * message in or out of a task's owned-message list, and that list is what the
 * scheduler scans when recomputing effective priority.
 */
static VOID kMesgSetOwner_(RK_MESG *const mesgPtr,
                           RK_TCB *const ownerPtr)
{
    RK_TCB *const oldOwnerPtr = mesgPtr->ownerPtr;

    if (oldOwnerPtr == ownerPtr)
    {
        return;
    }

    if (oldOwnerPtr != NULL)
    {
        RK_ERR const err =
            kListRemove(&oldOwnerPtr->asynchMesgOwnedList,
                        &mesgPtr->ownerNode);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }

    mesgPtr->ownerPtr = ownerPtr;
    mesgPtr->ownerNode.nextPtr = NULL;
    mesgPtr->ownerNode.prevPtr = NULL;

    if (ownerPtr != NULL)
    {
        RK_ERR const err =
            kListAddTail(&ownerPtr->asynchMesgOwnedList,
                         &mesgPtr->ownerNode);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }

    if (oldOwnerPtr != NULL)
    {
        /* Removing the message may drop the old owner's ceiling effect. */
        kTaskUpdateEffectivePrioChain(oldOwnerPtr);
    }

    if (ownerPtr != NULL)
    {
        /* Adding the message may apply the new owner's ceiling effect. */
        kTaskUpdateEffectivePrioChain(ownerPtr);
    }
}

/*
 * Allocated buffers are immediately owned by the allocator, so the pool
 * ceiling protects the whole fill/send window.
 */
static inline VOID kMesgInitAllocatedBuf_(RK_MESG *const mesgPtr,
                                          RK_MEM_PARTITION *const poolPtr,
                                          RK_TCB *const ownerPtr)
{
    if (mesgPtr->objID != RK_MESG_KOBJ_ID)
    {
        mesgPtr->ownerNode.nextPtr = NULL;
        mesgPtr->ownerNode.prevPtr = NULL;
        mesgPtr->ownerPtr = NULL;
    }

    mesgPtr->mesgNode.nextPtr = NULL;
    mesgPtr->mesgNode.prevPtr = NULL;
    mesgPtr->poolPtr = poolPtr;
    mesgPtr->sender = NULL;
    mesgPtr->receiver = NULL;
    mesgPtr->payloadBytes = poolPtr->blkSize - sizeof(RK_MESG);
    mesgPtr->senderPid = 0U;
    mesgPtr->receiverPid = 0U;
    mesgPtr->state = RK_MESG_STATE_ALLOCATED;
    mesgPtr->objID = RK_MESG_KOBJ_ID;
    kMesgSetOwner_(mesgPtr, ownerPtr);
}

static RK_ERR kMesgAllocFromPool_(RK_MEM_PARTITION *const poolPtr,
                                  RK_MESG **const mesgPPtr)
{
    RK_MESG *const mesgPtr = (RK_MESG *)kMemPartitionAlloc(poolPtr);
    if (mesgPtr == NULL)
    {
        return (RK_ERR_BUFFER_EMPTY);
    }

    kMesgInitAllocatedBuf_(mesgPtr, poolPtr, RK_gRunPtr);
    *mesgPPtr = mesgPtr;
    return (RK_ERR_SUCCESS);
}

/*
 * A freed message can satisfy a blocked kMesgAlloc() caller directly. Reuse
 * the buffer for the highest-priority waiting allocator instead of returning it
 * to the partition free list first; kMesgInitAllocatedBuf_() transfers the
 * ceiling ownership to that allocator.
 */
static RK_ERR kMesgHandoffToWaitingAllocator_(RK_MEM_PARTITION *const poolPtr,
                                              RK_MESG *const mesgPtr)
{
    RK_TCB *allocatorPtr = NULL;
    RK_ERR err = kTCBQDeq(&poolPtr->waitingQueue, &allocatorPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    if (allocatorPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
    {
        kRemoveTimeoutNode(&allocatorPtr->timeoutNode);
        allocatorPtr->timeoutNode.timeoutType = 0U;
    }
    allocatorPtr->timeoutNode.waitingQueuePtr = NULL;

    K_ASSERT(allocatorPtr->asynchMesgAllocDestPtr != NULL);
    if (allocatorPtr->asynchMesgAllocDestPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    kMesgInitAllocatedBuf_(mesgPtr, poolPtr, allocatorPtr);
    *(allocatorPtr->asynchMesgAllocDestPtr) = mesgPtr;
    kMesgClearAllocWait_(allocatorPtr);

    err = kMesgPublicReadyErr_(kReadySwtch(allocatorPtr));
    kTraceRecordObject(poolPtr, RK_TRACE_OP_WAKE, err,
                       poolPtr->waitingQueue.size);
    return (err);
}

static inline RK_BOOL kMesgSenderMatches_(RK_MESG const *const mesgPtr,
                                          RK_TCB const *const fromTaskPtr)
{
    if (fromTaskPtr == NULL)
    {
        return (RK_TRUE);
    }

    return (((mesgPtr->sender == kTaskHandleFromTcb(fromTaskPtr)) &&
             (mesgPtr->senderPid == fromTaskPtr->tid))
                ? RK_TRUE
                : RK_FALSE);
}

static RK_ERR kMesgResolveFilter_(RK_TASK_HANDLE const fromTaskHandle,
                                  RK_TCB **const fromTaskPPtr)
{
    if (fromTaskPPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    *fromTaskPPtr = NULL;

    if (fromTaskHandle == RK_ANY_TASK)
    {
        return (RK_ERR_SUCCESS);
    }

    if (fromTaskHandle == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    return (kTaskHandleResolve(fromTaskHandle, fromTaskPPtr));
}

static RK_MESG *kMesgDequeueMatching_(RK_TCB *const receiverPtr,
                                      RK_TCB const *const fromTaskPtr)
{
    RK_NODE *nodePtr = receiverPtr->asynchMesgQueue.listDummy.nextPtr;

    while (nodePtr != &receiverPtr->asynchMesgQueue.listDummy)
    {
        RK_NODE *const nextPtr = nodePtr->nextPtr;
        RK_MESG *const mesgPtr = K_GET_MESG_ADDR(nodePtr);

        if (kMesgSenderMatches_(mesgPtr, fromTaskPtr) == RK_TRUE)
        {
            RK_ERR const err =
                kListRemove(&receiverPtr->asynchMesgQueue, nodePtr);
            K_ASSERT(err == RK_ERR_SUCCESS);
            /*
             * No owner change here: send-time ownership already put this
             * message on the receiver's owned list for ceiling accounting.
             */
            mesgPtr->state = RK_MESG_STATE_RECEIVED;
            mesgPtr->receiver = kTaskHandleFromTcb(receiverPtr);
            mesgPtr->receiverPid = receiverPtr->tid;
            return (mesgPtr);
        }

        nodePtr = nextPtr;
        RK_BARRIER
    }

    return (NULL);
}

static RK_BOOL kMesgReceiverWaitMatches_(RK_TCB const *const receiverPtr,
                                         RK_MESG const *const mesgPtr)
{
    if ((receiverPtr == NULL) || (receiverPtr->asynchMesgWaitDestPtr == NULL))
    {
        return (RK_FALSE);
    }

    return (kMesgSenderMatches_(mesgPtr,
                                receiverPtr->asynchMesgWaitSenderPtr));
}

static RK_ERR kMesgDisarmBlockingTimeout_(RK_TCB *const taskPtr)
{
    if (taskPtr->timeoutNode.timeoutType != RK_TIMEOUT_BLOCKING)
    {
        return (RK_ERR_SUCCESS);
    }

    return (kTimeoutNodeDisarm(&taskPtr->timeoutNode));
}

static RK_BOOL kMesgDeliverToWaiter_(RK_TCB *const receiverPtr,
                                     RK_MESG *const mesgPtr,
                                     RK_ERR *const errPtr)
{
    if ((receiverPtr->asynchMesgWaiters.size == 0UL) ||
        (kMesgReceiverWaitMatches_(receiverPtr, mesgPtr) == RK_FALSE))
    {
        return (RK_FALSE);
    }

    RK_TCB *waiterPtr = kTCBQPeek(&receiverPtr->asynchMesgWaiters);
    K_ASSERT(waiterPtr == receiverPtr);
    RK_ERR err = kTCBQDeq(&receiverPtr->asynchMesgWaiters, &waiterPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
    if (err != RK_ERR_SUCCESS)
    {
        *errPtr = err;
        return (RK_TRUE);
    }

    err = kMesgDisarmBlockingTimeout_(receiverPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
    if (err != RK_ERR_SUCCESS)
    {
        *errPtr = err;
        return (RK_TRUE);
    }

    /*
     * Send-time ownership already moved the message to the receiver; direct
     * delivery only completes the wait.
     */
    mesgPtr->state = RK_MESG_STATE_RECEIVED;
    mesgPtr->receiver = kTaskHandleFromTcb(receiverPtr);
    mesgPtr->receiverPid = receiverPtr->tid;
    *(receiverPtr->asynchMesgWaitDestPtr) = mesgPtr;
    receiverPtr->asynchMesgWaitStatus = RK_ERR_SUCCESS;
    kMesgClearWait_(receiverPtr);

    *errPtr = kMesgPublicReadyErr_(kReadySwtch(receiverPtr));
    return (RK_TRUE);
}

static VOID kMesgReturnBufferFromCleanup_(RK_MESG *const mesgPtr)
{
    RK_MEM_PARTITION *const poolPtr =
        (mesgPtr != NULL) ? mesgPtr->poolPtr : NULL;

    if ((mesgPtr == NULL) || (poolPtr == NULL))
    {
        return;
    }

    if ((poolPtr->objID == RK_MEMALLOC_KOBJ_ID) &&
        (poolPtr->init == RK_TRUE) &&
        (poolPtr->waitingQueue.size > 0UL))
    {
        kMesgHandoffToWaitingAllocator_(poolPtr, mesgPtr);
        return;
    }

    kMesgSetOwner_(mesgPtr, NULL);
    mesgPtr->sender = NULL;
    mesgPtr->receiver = NULL;
    mesgPtr->senderPid = 0U;
    mesgPtr->receiverPid = 0U;
    mesgPtr->state = RK_MESG_STATE_FREE;
    mesgPtr->objID = RK_INVALID_KOBJ;

    if ((poolPtr->objID == RK_MEMALLOC_KOBJ_ID) &&
        (poolPtr->init == RK_TRUE))
    {
        kMemPartitionFree(poolPtr, mesgPtr);
    }
}

VOID kMesgTaskCleanup(RK_TCB *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return;
    }

    while (taskPtr->asynchMesgQueue.size > 0UL)
    {
        RK_NODE *nodePtr = NULL;
        RK_ERR err = kListRemoveHead(&taskPtr->asynchMesgQueue, &nodePtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if ((err != RK_ERR_SUCCESS) || (nodePtr == NULL))
        {
            break;
        }

        RK_MESG *const mesgPtr = K_GET_MESG_ADDR(nodePtr);
        kMesgReturnBufferFromCleanup_(mesgPtr);
    }

    while (taskPtr->asynchMesgOwnedList.size > 0UL)
    {
        RK_NODE *const nodePtr =
            taskPtr->asynchMesgOwnedList.listDummy.nextPtr;
        if (nodePtr == &taskPtr->asynchMesgOwnedList.listDummy)
        {
            break;
        }

        RK_MESG *const mesgPtr =
            K_GET_CONTAINER_ADDR(nodePtr, RK_MESG, ownerNode);
        kMesgReturnBufferFromCleanup_(mesgPtr);
    }

    while (taskPtr->asynchMesgWaiters.size > 0UL)
    {
        RK_TCB *waiterPtr = NULL;
        RK_ERR err = kTCBQDeq(&taskPtr->asynchMesgWaiters, &waiterPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if (err != RK_ERR_SUCCESS)
        {
            break;
        }

        if (waiterPtr == taskPtr)
        {
            continue;
        }

        if (waiterPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
        {
            kTimeoutNodeDisarm(&waiterPtr->timeoutNode);
        }
        waiterPtr->asynchMesgWaitStatus = RK_ERR_OBJ_NOT_INIT;
        waiterPtr->asynchMesgWaitSenderPtr = NULL;
        waiterPtr->asynchMesgWaitDestPtr = NULL;
        waiterPtr->timeOut = RK_TRUE;
        kReadySwtch(waiterPtr);
    }

    taskPtr->asynchMesgInit = RK_FALSE;
    kListInit(&taskPtr->asynchMesgQueue);
    kListInit(&taskPtr->asynchMesgWaiters);
    kListInit(&taskPtr->asynchMesgOwnedList);
    kMesgClearWait_(taskPtr);
    kMesgClearAllocWait_(taskPtr);

#if (RK_CONF_ASYNCH_COPY_MESG == ON)
    while (taskPtr->asynchCopyMesgQueue.size > 0UL)
    {
        RK_NODE *nodePtr = NULL;
        RK_ERR err =
            kListRemoveHead(&taskPtr->asynchCopyMesgQueue, &nodePtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if ((err != RK_ERR_SUCCESS) || (nodePtr == NULL))
        {
            break;
        }

        kMesgCopyFree_(K_GET_MESG_ADDR(nodePtr));
    }

    while (taskPtr->asynchCopyMesgWaiters.size > 0UL)
    {
        RK_TCB *waiterPtr = NULL;
        RK_ERR err =
            kTCBQDeq(&taskPtr->asynchCopyMesgWaiters, &waiterPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if (err != RK_ERR_SUCCESS)
        {
            break;
        }

        if (waiterPtr == taskPtr)
        {
            if (kTimeoutNodeIsArmed(&taskPtr->timeoutNode) == RK_TRUE)
            {
                kTimeoutNodeDisarm(&taskPtr->timeoutNode);
            }
            else
            {
                kTimeoutNodeReset(&taskPtr->timeoutNode);
            }
            kMesgCopyClearWait_(taskPtr);
        }
        else if (waiterPtr != NULL)
        {
            kMesgCopyReadyWithStatus_(waiterPtr, RK_ERR_OBJ_NOT_INIT);
        }
    }

    kMesgCopyCancelWaitersForSender_(taskPtr);
    taskPtr->asynchCopyMesgInit = RK_FALSE;
    kListInit(&taskPtr->asynchCopyMesgQueue);
    kListInit(&taskPtr->asynchCopyMesgWaiters);
    kMesgCopyClearWait_(taskPtr);
#endif
}

RK_ERR kMesgEndpointInit(RK_TASK_HANDLE const taskHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_ENDPOINT_INIT,
                                        (ULONG)(UINTPTR)taskHandle,
                                        0UL, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if (taskHandle == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (kIsISR())
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }
#endif

    if (kIsISR())
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (taskHandle == NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR resolveErr = kTaskHandleResolve(taskHandle, &taskPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (resolveErr);
    }

    if (taskPtr->init != RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (taskPtr->asynchMesgInit == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_DOUBLE_INIT);
    }

#if (RK_CONF_ASYNCH_COPY_MESG == ON)
    if (taskPtr->asynchCopyMesgInit == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_HAS_OWNER);
    }
#endif

#if (RK_CONF_SYNCH_MESG == ON)
    if (taskPtr->synchMesgMaxBytes != 0UL)
    {
        RK_CR_EXIT
        return (RK_ERR_HAS_OWNER);
    }
#endif

    taskPtr->asynchMesgInit = RK_TRUE;
    kListInit(&taskPtr->asynchMesgQueue);
    kListInit(&taskPtr->asynchMesgWaiters);
    kMesgClearWait_(taskPtr);

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

#if (RK_CONF_ASYNCH_COPY_MESG == ON)
RK_ERR kMesgCopyEndpointInit(RK_TASK_HANDLE const taskHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_COPY_ENDPOINT_INIT,
                                        (ULONG)(UINTPTR)taskHandle,
                                        0UL, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

    if (kIsISR())
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (taskHandle == NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR err = kTaskHandleResolve(taskHandle, &taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    if (taskPtr->init != RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (taskPtr->asynchCopyMesgInit == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_DOUBLE_INIT);
    }

    if (taskPtr->asynchMesgInit == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_HAS_OWNER);
    }

#if (RK_CONF_SYNCH_MESG == ON)
    if (taskPtr->synchMesgMaxBytes != 0UL)
    {
        RK_CR_EXIT
        return (RK_ERR_HAS_OWNER);
    }
#endif

    err = kMesgCopyPoolEnsureInit_();
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    taskPtr->asynchCopyMesgInit = RK_TRUE;
    kListInit(&taskPtr->asynchCopyMesgQueue);
    kListInit(&taskPtr->asynchCopyMesgWaiters);
    kMesgCopyClearWait_(taskPtr);

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgSendCopy(RK_TASK_HANDLE const taskHandle,
                     VOID const *const sendPtr,
                     ULONG const bytes)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_SEND_COPY,
                                        (ULONG)(UINTPTR)taskHandle,
                                        (ULONG)(UINTPTR)sendPtr,
                                        bytes, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

    if ((taskHandle == NULL) || (sendPtr == NULL))
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (taskHandle == RK_ANY_TASK)
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    if ((kIsISR()) || (RK_gRunPtr == NULL))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((bytes == 0UL) ||
        (bytes > RK_CONF_ASYNCH_COPY_MESG_PAYLOAD_BYTES))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_MSG_SIZE);
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR err = kTaskHandleResolve(taskHandle, &taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    if (taskPtr->init != RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (taskPtr->asynchCopyMesgInit != RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if ((RK_gSyscallThreadModeActive != 0U) &&
        (kMpuUserReadValid(RK_gRunPtr, sendPtr, bytes) != RK_TRUE))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    if (kMesgCopyDeliverPayloadToWaiter_(taskPtr, RK_gRunPtr, sendPtr,
                                         bytes, &err) == RK_TRUE)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kMesgCopyStore_(taskPtr, RK_gRunPtr, taskHandle, sendPtr, bytes);
    RK_CR_EXIT
    return (err);
}

RK_ERR kMesgRecvCopy(RK_TASK_HANDLE const fromTaskHandle,
                     VOID *const recvPtr,
                     ULONG const recvBytes,
                     ULONG *const rxBytesPtr,
                     RK_TICK const timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        RK_MESG_RECV_COPY_SYSCALL_ARGS syscallArgs;

        syscallArgs.fromTaskHandle = fromTaskHandle;
        syscallArgs.recvPtr = recvPtr;
        syscallArgs.recvBytes = recvBytes;
        syscallArgs.rxBytesPtr = rxBytesPtr;
        syscallArgs.timeout = timeout;

        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_RECV_COPY,
                                        (ULONG)(UINTPTR)&syscallArgs,
                                        0UL, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

    if (recvPtr == NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if ((kIsISR()) || (RK_gRunPtr == NULL))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (recvBytes == 0UL)
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_MSG_SIZE);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_TIMEOUT);
    }

    if (rxBytesPtr != NULL)
    {
        *rxBytesPtr = 0UL;
    }

    if (RK_gRunPtr->asynchCopyMesgInit != RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    RK_TCB *fromTaskPtr = NULL;
    RK_ERR err = kMesgResolveFilter_(fromTaskHandle, &fromTaskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    while (RK_TRUE)
    {
        RK_MESG *const mesgPtr =
            kMesgCopyDequeueMatching_(RK_gRunPtr, fromTaskPtr, recvBytes);
        if (mesgPtr != NULL)
        {
            RK_MEMCPY(recvPtr, kMesgPayloadRawConst_(mesgPtr),
                      mesgPtr->payloadBytes);
            if (rxBytesPtr != NULL)
            {
                *rxBytesPtr = mesgPtr->payloadBytes;
            }
            kMesgCopyFree_(mesgPtr);
            RK_CR_EXIT
            return (RK_ERR_SUCCESS);
        }

        if (timeout == RK_NO_WAIT)
        {
            RK_CR_EXIT
            return (RK_ERR_BUFFER_EMPTY);
        }

        RK_gRunPtr->timeoutNode.waitingQueuePtr =
            &RK_gRunPtr->asynchCopyMesgWaiters;
        if (timeout != RK_WAIT_FOREVER)
        {
            RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
            err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
                RK_CR_EXIT
                return (err);
            }
        }

        RK_gRunPtr->status = RK_RECEIVING;
        RK_gRunPtr->asynchCopyMesgWaitSenderPtr = fromTaskPtr;
        RK_gRunPtr->asynchCopyMesgRecvBufPtr = recvPtr;
        RK_gRunPtr->asynchCopyMesgRecvBufBytes = recvBytes;
        RK_gRunPtr->asynchCopyMesgRecvBytesPtr = rxBytesPtr;
        RK_gRunPtr->asynchCopyMesgRecvStatus = RK_ERR_SUCCESS;
        err = kTCBQEnq(&RK_gRunPtr->asynchCopyMesgWaiters, RK_gRunPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if (err != RK_ERR_SUCCESS)
        {
            if (timeout != RK_WAIT_FOREVER)
            {
                RK_ERR const disarmErr =
                    kTimeoutNodeDisarm(&RK_gRunPtr->timeoutNode);
                K_ASSERT(disarmErr == RK_ERR_SUCCESS);
            }
            kMesgCopyClearWait_(RK_gRunPtr);
            RK_gRunPtr->status = RK_RUNNING;
            RK_CR_EXIT
            return (err);
        }

        kPendCtxSwtch();
        RK_CR_EXIT
        RK_CR_ENTER

        if (RK_gRunPtr->timeOut)
        {
            RK_gRunPtr->timeOut = RK_FALSE;
            kMesgCopyClearWait_(RK_gRunPtr);
            RK_CR_EXIT
            return (RK_ERR_TIMEOUT);
        }

        err = RK_gRunPtr->asynchCopyMesgRecvStatus;
        kMesgCopyClearWait_(RK_gRunPtr);
        RK_CR_EXIT
        return (err);
    }
}

RK_ERR kMesgRecvCopySyscall(
    RK_EXCEPTION_FRAME *const framePtr,
    ULONG const userArgsAddr,
    RK_MESG_RECV_COPY_SYSCALL_ARGS const *const argsPtr)
{
    if (argsPtr == NULL)
    {
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    RK_CR_AREA
    RK_CR_ENTER

    if ((argsPtr->recvPtr == NULL) || (RK_gRunPtr == NULL))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    if (argsPtr->recvBytes == 0UL)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_MSG_SIZE);
    }

    if ((argsPtr->timeout != RK_WAIT_FOREVER) &&
        (argsPtr->timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }

    if (RK_gRunPtr->asynchCopyMesgInit != RK_TRUE)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (argsPtr->rxBytesPtr != NULL)
    {
        *(argsPtr->rxBytesPtr) = 0UL;
    }

    RK_TCB *fromTaskPtr = NULL;
    RK_ERR err = kMesgResolveFilter_(argsPtr->fromTaskHandle, &fromTaskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (err);
    }

    RK_MESG *const mesgPtr =
        kMesgCopyDequeueMatching_(RK_gRunPtr, fromTaskPtr, argsPtr->recvBytes);
    if (mesgPtr != NULL)
    {
        RK_MEMCPY(argsPtr->recvPtr, kMesgPayloadRawConst_(mesgPtr),
                  mesgPtr->payloadBytes);
        if (argsPtr->rxBytesPtr != NULL)
        {
            *(argsPtr->rxBytesPtr) = mesgPtr->payloadBytes;
        }
        kMesgCopyFree_(mesgPtr);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_SUCCESS);
    }

    if (argsPtr->timeout == RK_NO_WAIT)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_gRunPtr->timeoutNode.waitingQueuePtr =
        &RK_gRunPtr->asynchCopyMesgWaiters;
    if (argsPtr->timeout != RK_WAIT_FOREVER)
    {
        RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
        err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, argsPtr->timeout);
        if (err != RK_ERR_SUCCESS)
        {
            kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (err);
        }
    }

    RK_gRunPtr->status = RK_RECEIVING;
    RK_gRunPtr->asynchCopyMesgWaitSenderPtr = fromTaskPtr;
    RK_gRunPtr->asynchCopyMesgRecvBufPtr = argsPtr->recvPtr;
    RK_gRunPtr->asynchCopyMesgRecvBufBytes = argsPtr->recvBytes;
    RK_gRunPtr->asynchCopyMesgRecvBytesPtr = argsPtr->rxBytesPtr;
    RK_gRunPtr->asynchCopyMesgRecvStatus = RK_ERR_SUCCESS;
    err = kTCBQEnq(&RK_gRunPtr->asynchCopyMesgWaiters, RK_gRunPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
    if (err != RK_ERR_SUCCESS)
    {
        if (argsPtr->timeout != RK_WAIT_FOREVER)
        {
            RK_ERR const disarmErr =
                kTimeoutNodeDisarm(&RK_gRunPtr->timeoutNode);
            K_ASSERT(disarmErr == RK_ERR_SUCCESS);
        }
        kMesgCopyClearWait_(RK_gRunPtr);
        RK_gRunPtr->status = RK_RUNNING;
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (err);
    }

    kSyscallTaskSuspend(framePtr, RK_SYSCALL_MESG_RECV_COPY, userArgsAddr,
                        0UL, 0UL, 0UL, RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}
#endif

static RK_ERR kMesgPoolInitWithAttr_(RK_MEM_PARTITION *const poolPtr,
                                     VOID *const memPoolPtr,
                                     ULONG const payloadBytes,
                                     ULONG const nMesg,
                                     RK_PRIO const ceilingPrio,
                                     RK_OBJ_ATTR const *const attrPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    RK_ERR const phaseErr = kKernelRawInitGuard();
    if (phaseErr != RK_ERR_SUCCESS)
    {
        return (phaseErr);
    }

    ULONG const blockBytes = kMesgBlockBytes_(payloadBytes);

    if (blockBytes == 0UL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }

    /*
     * RK priorities are numerically inverted: a lower number is a higher
     * priority. The ceiling must be a valid task priority or the NONE sentinel.
     */
    if ((ceilingPrio != RK_MESG_PRIO_CEILING_NONE) &&
        (ceilingPrio > RK_CONF_MIN_PRIO))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_TASK_INVALID_PRIO);
#endif
        return (RK_ERR_INVALID_PRIO);
    }

    RK_ERR const err =
        (attrPtr != NULL)
            ? ((attrPtr->scope == RK_SCOPE_KERNEL_GLOBAL)
                   ? kMemPartitionInitGlobalScope(poolPtr, memPoolPtr,
                                                  blockBytes, nMesg)
                   : kMemPartitionInitDomainScope(poolPtr, memPoolPtr,
                                                  blockBytes, nMesg,
                                                  attrPtr->domainPtr))
            : kMemPartitionInit(poolPtr, memPoolPtr, blockBytes, nMesg);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    /*
     * kMemPartitionInit() leaves the generic partition ceiling-disabled.
     * Message pools opt in here when the caller supplied a real ceiling.
     */
    if (ceilingPrio == RK_MESG_PRIO_CEILING_NONE)
    {
        poolPtr->mesgPrioCeiling = RK_MESG_PRIO_CEILING_NONE;
        poolPtr->mesgPrioCeilingEnabled = RK_FALSE;
    }
    else
    {
        poolPtr->mesgPrioCeiling = ceilingPrio;
        poolPtr->mesgPrioCeilingEnabled = RK_TRUE;
    }

    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgPoolInit(RK_MEM_PARTITION *const poolPtr,
                     VOID *const memPoolPtr,
                     ULONG const payloadBytes,
                     ULONG const nMesg,
                     RK_PRIO const ceilingPrio)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        RK_MESG_POOL_INIT_SYSCALL_ARGS syscallArgs;

        syscallArgs.poolPtr = poolPtr;
        syscallArgs.memPoolPtr = memPoolPtr;
        syscallArgs.payloadBytes = payloadBytes;
        syscallArgs.nMesg = nMesg;
        syscallArgs.ceilingPrio = ceilingPrio;

        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_POOL_INIT,
                                        (ULONG)(UINTPTR)&syscallArgs,
                                        0UL, 0UL, 0UL));
    }

    return (kMesgPoolInitWithAttr_(poolPtr, memPoolPtr, payloadBytes, nMesg,
                                   ceilingPrio, NULL));
}

RK_ERR kMesgPoolInitGlobalScope(RK_MEM_PARTITION *const poolPtr,
                                VOID *const memPoolPtr,
                                ULONG const payloadBytes,
                                ULONG const nMesg,
                                RK_PRIO const ceilingPrio)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_KERNEL_GLOBAL, NULL };
    return (kMesgPoolInitWithAttr_(poolPtr, memPoolPtr, payloadBytes, nMesg,
                                   ceilingPrio, &attr));
}

RK_ERR kMesgPoolInitDomainScope(RK_MEM_PARTITION *const poolPtr,
                                VOID *const memPoolPtr,
                                ULONG const payloadBytes,
                                ULONG const nMesg,
                                RK_PRIO const ceilingPrio,
                                RK_DOMAIN *const domainPtr)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_DOMAIN_LOCAL, domainPtr };
    return (kMesgPoolInitWithAttr_(poolPtr, memPoolPtr, payloadBytes, nMesg,
                                   ceilingPrio, &attr));
}

RK_ERR kMesgAlloc(RK_MEM_PARTITION *const poolPtr,
                  RK_MESG **const mesgPPtr,
                  RK_TICK const timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_ALLOC,
                                        (ULONG)(UINTPTR)poolPtr,
                                        (ULONG)(UINTPTR)mesgPPtr,
                                        (ULONG)timeout, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if ((poolPtr == NULL) || (mesgPPtr == NULL))
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (poolPtr->objID != RK_MEMALLOC_KOBJ_ID)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    if (poolPtr->init != RK_TRUE)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (K_BLOCKING_ON_ISR(timeout))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
        RK_CR_EXIT
        return (RK_ERR_INVALID_TIMEOUT);
    }
#endif

    if (mesgPPtr == NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }
    *mesgPPtr = NULL;

    if (poolPtr == NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (poolPtr->objID != RK_MEMALLOC_KOBJ_ID)
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    if (poolPtr->init != RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (poolPtr->blkSize <= sizeof(RK_MESG))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    if ((kIsISR()) && (timeout != RK_NO_WAIT))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((RK_gRunPtr == NULL) && (timeout != RK_NO_WAIT))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_TIMEOUT);
    }

    RK_ERR err = kMesgAllocFromPool_(poolPtr, mesgPPtr);
    if (err == RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }
    if ((err != RK_ERR_BUFFER_EMPTY) || (timeout == RK_NO_WAIT))
    {
        RK_CR_EXIT
        return (err);
    }

    while (*mesgPPtr == NULL)
    {
        RK_gRunPtr->timeoutNode.waitingQueuePtr = &poolPtr->waitingQueue;
        if (timeout != RK_WAIT_FOREVER)
        {
            RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
            RK_ERR const timeoutErr =
                kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (timeoutErr != RK_ERR_SUCCESS)
            {
                kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
                RK_CR_EXIT
                return (timeoutErr);
            }
        }

        RK_gRunPtr->status = RK_BLOCKED;
        RK_gRunPtr->asynchMesgAllocDestPtr = mesgPPtr;
        kTraceRecordObject(poolPtr, RK_TRACE_OP_WAIT_BLOCK, RK_ERR_SUCCESS,
                           poolPtr->waitingQueue.size + 1UL);
        err = kTCBQEnqByPrio(&poolPtr->waitingQueue, RK_gRunPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if (err != RK_ERR_SUCCESS)
        {
            if (timeout != RK_WAIT_FOREVER)
            {
                RK_ERR const disarmErr =
                    kTimeoutNodeDisarm(&RK_gRunPtr->timeoutNode);
                K_ASSERT(disarmErr == RK_ERR_SUCCESS);
            }
            kMesgClearAllocWait_(RK_gRunPtr);
            RK_gRunPtr->status = RK_RUNNING;
            RK_CR_EXIT
            return (err);
        }

        kPendCtxSwtch();
        RK_CR_EXIT
        RK_CR_ENTER

        if (RK_gRunPtr->timeOut)
        {
            RK_gRunPtr->timeOut = RK_FALSE;
            kMesgClearAllocWait_(RK_gRunPtr);
            kTraceRecordObject(poolPtr, RK_TRACE_OP_TIMEOUT, RK_ERR_TIMEOUT,
                               poolPtr->waitingQueue.size);
            RK_CR_EXIT
            return (RK_ERR_TIMEOUT);
        }

        if (*mesgPPtr != NULL)
        {
            kTraceRecordObject(poolPtr, RK_TRACE_OP_ALLOC, RK_ERR_SUCCESS,
                               poolPtr->nFreeBlocks);
            RK_CR_EXIT
            return (RK_ERR_SUCCESS);
        }

        err = kMesgAllocFromPool_(poolPtr, mesgPPtr);
        if (err == RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (RK_ERR_SUCCESS);
        }
        if (err != RK_ERR_BUFFER_EMPTY)
        {
            RK_CR_EXIT
            return (err);
        }
    }

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgAllocSyscall(RK_EXCEPTION_FRAME *const framePtr,
                         RK_MEM_PARTITION *const poolPtr,
                         RK_MESG **const mesgPPtr,
                         RK_TICK const timeout)
{
    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if ((poolPtr == NULL) || (mesgPPtr == NULL))
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    if (poolPtr->objID != RK_MEMALLOC_KOBJ_ID)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_OBJ);
    }

    if (poolPtr->init != RK_TRUE)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }
#endif

    if (mesgPPtr == NULL)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }
    *mesgPPtr = NULL;

    if (poolPtr == NULL)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    if (poolPtr->objID != RK_MEMALLOC_KOBJ_ID)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_OBJ);
    }

    if (poolPtr->init != RK_TRUE)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (poolPtr->blkSize <= sizeof(RK_MESG))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_OBJ);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }

    RK_ERR err = kMesgAllocFromPool_(poolPtr, mesgPPtr);
    if (err == RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_SUCCESS);
    }

    if ((err != RK_ERR_BUFFER_EMPTY) || (timeout == RK_NO_WAIT))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (err);
    }

    RK_gRunPtr->timeoutNode.waitingQueuePtr = &poolPtr->waitingQueue;
    if (timeout != RK_WAIT_FOREVER)
    {
        RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
        err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (err);
        }
    }

    RK_gRunPtr->status = RK_BLOCKED;
    RK_gRunPtr->asynchMesgAllocDestPtr = mesgPPtr;
    kTraceRecordObject(poolPtr, RK_TRACE_OP_WAIT_BLOCK, RK_ERR_SUCCESS,
                       poolPtr->waitingQueue.size + 1UL);
    err = kTCBQEnqByPrio(&poolPtr->waitingQueue, RK_gRunPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
    if (err != RK_ERR_SUCCESS)
    {
        if (timeout != RK_WAIT_FOREVER)
        {
            RK_ERR const disarmErr =
                kTimeoutNodeDisarm(&RK_gRunPtr->timeoutNode);
            K_ASSERT(disarmErr == RK_ERR_SUCCESS);
        }
        kMesgClearAllocWait_(RK_gRunPtr);
        RK_gRunPtr->status = RK_RUNNING;
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (err);
    }

    kSyscallTaskSuspend(framePtr, RK_SYSCALL_MESG_ALLOC,
                        (ULONG)(UINTPTR)poolPtr,
                        (ULONG)(UINTPTR)mesgPPtr, (ULONG)timeout, 0UL,
                        RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

RK_ERR kMesgFree(RK_MESG *const mesgPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_FREE,
                                        (ULONG)(UINTPTR)mesgPtr,
                                        0UL, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if (mesgPtr == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (mesgPtr->objID != RK_MESG_KOBJ_ID)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }
#endif

    if (mesgPtr == NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (kMesgIsValid_(mesgPtr) == RK_FALSE)
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    if (RK_gSyscallThreadModeActive != 0U)
    {
        RK_ERR const userErr = kMesgUserBlockWriteValid_(RK_gRunPtr,
                                                         mesgPtr);
        if (userErr != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (userErr);
        }
    }

    if (kMesgStateOwned_(mesgPtr) == RK_FALSE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_MESG_INVALID_STATE);
#endif
        RK_CR_EXIT
        return (RK_ERR_MESG_INVALID_STATE);
    }

    RK_MEM_PARTITION *const poolPtr = mesgPtr->poolPtr;
    if (poolPtr == NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    if (poolPtr->waitingQueue.size > 0UL)
    {
        /*
         * A waiting allocator takes ownership immediately, so the ceiling moves
         * directly from the freeing task to that allocator.
         */
        RK_ERR const err = kMesgHandoffToWaitingAllocator_(poolPtr, mesgPtr);
        RK_CR_EXIT
        return (err);
    }

    /*
     * No allocator is waiting. Clear ownership before returning the buffer to
     * the pool so the freeing task loses this pool's ceiling contribution.
     */
    kMesgSetOwner_(mesgPtr, NULL);
    mesgPtr->sender = NULL;
    mesgPtr->receiver = NULL;
    mesgPtr->senderPid = 0U;
    mesgPtr->receiverPid = 0U;
    mesgPtr->state = RK_MESG_STATE_FREE;
    mesgPtr->objID = RK_INVALID_KOBJ;
    RK_ERR const err = kMemPartitionFree(poolPtr, mesgPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
    RK_CR_EXIT
    return (err);
}

VOID *kMesgPayload(RK_MESG *const mesgPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((VOID *)(UINTPTR)kSyscallInvoke4(RK_SYSCALL_MESG_PAYLOAD,
                                                (ULONG)(UINTPTR)mesgPtr,
                                                0UL, 0UL, 0UL));
    }

    if (kMesgIsValid_(mesgPtr) == RK_FALSE)
    {
        return (NULL);
    }

    if ((RK_gSyscallThreadModeActive != 0U) &&
        (kMesgUserBlockWriteValid_(RK_gRunPtr, mesgPtr) != RK_ERR_SUCCESS))
    {
        return (NULL);
    }

    return ((VOID *)((BYTE *)mesgPtr + sizeof(RK_MESG)));
}

VOID const *kMesgPayloadConst(RK_MESG const *const mesgPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((VOID const *)(UINTPTR)kSyscallInvoke4(
            RK_SYSCALL_MESG_PAYLOAD_CONST, (ULONG)(UINTPTR)mesgPtr,
            0UL, 0UL, 0UL));
    }

    if (kMesgIsValid_(mesgPtr) == RK_FALSE)
    {
        return (NULL);
    }

    if ((RK_gSyscallThreadModeActive != 0U) &&
        (kMesgUserBlockWriteValid_(RK_gRunPtr, mesgPtr) != RK_ERR_SUCCESS))
    {
        return (NULL);
    }

    return ((VOID const *)((BYTE const *)mesgPtr + sizeof(RK_MESG)));
}

ULONG kMesgPayloadBytes(RK_MESG const *const mesgPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (kSyscallInvoke4(RK_SYSCALL_MESG_PAYLOAD_BYTES,
                               (ULONG)(UINTPTR)mesgPtr, 0UL, 0UL, 0UL));
    }

    if (kMesgIsValid_(mesgPtr) == RK_FALSE)
    {
        return (0UL);
    }

    if ((RK_gSyscallThreadModeActive != 0U) &&
        (kMesgUserBlockWriteValid_(RK_gRunPtr, mesgPtr) != RK_ERR_SUCCESS))
    {
        return (0UL);
    }

    return (mesgPtr->payloadBytes);
}

RK_TASK_HANDLE kMesgGetSenderHandle(RK_MESG const *const mesgPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_TASK_HANDLE)(UINTPTR)kSyscallInvoke4(
            RK_SYSCALL_MESG_GET_SENDER_HANDLE, (ULONG)(UINTPTR)mesgPtr,
            0UL, 0UL, 0UL));
    }

    if (kMesgIsValid_(mesgPtr) == RK_FALSE)
    {
        return (NULL);
    }

    return (mesgPtr->sender);
}

RK_ERR kMesgGetSenderID(RK_MESG const *const mesgPtr,
                        RK_TID *const senderIDPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_GET_SENDER_ID,
                                        (ULONG)(UINTPTR)mesgPtr,
                                        (ULONG)(UINTPTR)senderIDPtr,
                                        0UL, 0UL));
    }

#if (RK_CONF_ERR_CHECK == ON)
    if ((mesgPtr == NULL) || (senderIDPtr == NULL))
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        return (RK_ERR_OBJ_NULL);
    }

    if (mesgPtr->objID != RK_MESG_KOBJ_ID)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
        return (RK_ERR_INVALID_OBJ);
    }
#endif

    if ((mesgPtr == NULL) || (senderIDPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (kMesgIsValid_(mesgPtr) == RK_FALSE)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if (kMesgStateHasSender_(mesgPtr) == RK_FALSE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_MESG_INVALID_STATE);
#endif
        return (RK_ERR_MESG_INVALID_STATE);
    }

    *senderIDPtr = mesgPtr->senderPid;
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgSend(RK_TASK_HANDLE const taskHandle,
                 RK_MESG *const mesgPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_SEND,
                                        (ULONG)(UINTPTR)taskHandle,
                                        (ULONG)(UINTPTR)mesgPtr,
                                        0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if ((taskHandle == NULL) || (mesgPtr == NULL))
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (taskHandle == RK_ANY_TASK)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    if (kIsISR())
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }
#endif

    if ((taskHandle == NULL) || (mesgPtr == NULL))
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (taskHandle == RK_ANY_TASK)
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    if ((kIsISR()) || (RK_gRunPtr == NULL))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR resolveErr = kTaskHandleResolve(taskHandle, &taskPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (resolveErr);
    }

    if (taskPtr->init != RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (taskPtr->asynchMesgInit != RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (kMesgTasksShareDomain_(RK_gRunPtr, taskPtr) != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    if (kMesgIsValid_(mesgPtr) == RK_FALSE)
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    if (RK_gSyscallThreadModeActive != 0U)
    {
        RK_ERR userErr = kMesgUserBlockWriteValid_(RK_gRunPtr, mesgPtr);
        if (userErr != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (userErr);
        }

        userErr = kMesgUserBlockWriteValid_(taskPtr, mesgPtr);
        if (userErr != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (userErr);
        }
    }

    if (kMesgStateOwned_(mesgPtr) == RK_FALSE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_MESG_INVALID_STATE);
#endif
        RK_CR_EXIT
        return (RK_ERR_MESG_INVALID_STATE);
    }

    mesgPtr->sender = kTaskHandleFromTcb(RK_gRunPtr);
    mesgPtr->senderPid = RK_gRunPtr->tid;
    mesgPtr->receiver = taskHandle;
    mesgPtr->receiverPid = taskPtr->tid;
    /*
     * The sender gives up ceiling ownership at send time. The receiver owns the
     * message while it is queued and while it processes the received buffer.
     */
    kMesgSetOwner_(mesgPtr, taskPtr);

    RK_ERR err = RK_ERR_SUCCESS;
    if (kMesgDeliverToWaiter_(taskPtr, mesgPtr, &err) == RK_TRUE)
    {
        kTraceRecordObject(mesgPtr->poolPtr, RK_TRACE_OP_SEND, err,
                           taskPtr->asynchMesgQueue.size);
        RK_CR_EXIT
        return (err);
    }

    mesgPtr->state = RK_MESG_STATE_QUEUED;
    err = kListAddTail(&taskPtr->asynchMesgQueue, &mesgPtr->mesgNode);
    kTraceRecordObject(mesgPtr->poolPtr, RK_TRACE_OP_SEND, err,
                       taskPtr->asynchMesgQueue.size);

    RK_CR_EXIT
    return (err);
}

RK_ERR kMesgWait(RK_TASK_HANDLE const fromTaskHandle,
                 RK_MESG **const mesgPPtr,
                 RK_TICK const timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_WAIT,
                                        (ULONG)(UINTPTR)fromTaskHandle,
                                        (ULONG)(UINTPTR)mesgPPtr,
                                        (ULONG)timeout, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if (mesgPPtr == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if ((kIsISR()) || (RK_gRunPtr == NULL))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (RK_gRunPtr->asynchMesgInit != RK_TRUE)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
        RK_CR_EXIT
        return (RK_ERR_INVALID_TIMEOUT);
    }
#endif

    if (mesgPPtr == NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if ((kIsISR()) || (RK_gRunPtr == NULL))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    *mesgPPtr = NULL;

    RK_TCB *fromTaskPtr = NULL;
    RK_ERR err = kMesgResolveFilter_(fromTaskHandle, &fromTaskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    if (RK_gRunPtr->asynchMesgInit != RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if ((fromTaskPtr != NULL) &&
        (kMesgTasksShareDomain_(RK_gRunPtr, fromTaskPtr) != RK_TRUE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_TIMEOUT);
    }

    while (*mesgPPtr == NULL)
    {
        *mesgPPtr = kMesgDequeueMatching_(RK_gRunPtr, fromTaskPtr);
        if (*mesgPPtr != NULL)
        {
            kTraceRecordObject((*mesgPPtr)->poolPtr, RK_TRACE_OP_RECV,
                               RK_ERR_SUCCESS,
                               RK_gRunPtr->asynchMesgQueue.size);
            RK_CR_EXIT
            return (RK_ERR_SUCCESS);
        }

        if (timeout == RK_NO_WAIT)
        {
            RK_CR_EXIT
            return (RK_ERR_BUFFER_EMPTY);
        }

        if (timeout != RK_WAIT_FOREVER)
        {
            RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
            RK_gRunPtr->timeoutNode.waitingQueuePtr =
                &RK_gRunPtr->asynchMesgWaiters;
            RK_ERR const timeoutErr =
                kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (timeoutErr != RK_ERR_SUCCESS)
            {
                kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
                RK_CR_EXIT
                return (timeoutErr);
            }
        }

        RK_gRunPtr->status = RK_RECEIVING;
        RK_gRunPtr->asynchMesgWaitSenderPtr = fromTaskPtr;
        RK_gRunPtr->asynchMesgWaitDestPtr = mesgPPtr;
        RK_gRunPtr->asynchMesgWaitStatus = RK_ERR_SUCCESS;
        err = kTCBQEnq(&RK_gRunPtr->asynchMesgWaiters, RK_gRunPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if (err != RK_ERR_SUCCESS)
        {
            if (timeout != RK_WAIT_FOREVER)
            {
                RK_ERR const disarmErr =
                    kTimeoutNodeDisarm(&RK_gRunPtr->timeoutNode);
                K_ASSERT(disarmErr == RK_ERR_SUCCESS);
            }
            kMesgClearWait_(RK_gRunPtr);
            RK_gRunPtr->status = RK_RUNNING;
            RK_CR_EXIT
            return (err);
        }

        kPendCtxSwtch();
        RK_CR_EXIT
        RK_CR_ENTER

        if (RK_gRunPtr->timeOut)
        {
            RK_gRunPtr->timeOut = RK_FALSE;
            kMesgClearWait_(RK_gRunPtr);
            RK_CR_EXIT
            return (RK_ERR_TIMEOUT);
        }

        if (*mesgPPtr != NULL)
        {
            kTraceRecordObject((*mesgPPtr)->poolPtr, RK_TRACE_OP_RECV,
                               RK_ERR_SUCCESS,
                               RK_gRunPtr->asynchMesgQueue.size);
            kMesgClearWait_(RK_gRunPtr);
            RK_CR_EXIT
            return (RK_ERR_SUCCESS);
        }
    }

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgWaitSyscall(RK_EXCEPTION_FRAME *const framePtr,
                        RK_TASK_HANDLE const fromTaskHandle,
                        RK_MESG **const mesgPPtr,
                        RK_TICK const timeout)
{
    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if (mesgPPtr == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    if ((RK_gRunPtr == NULL) ||
        (RK_gRunPtr->asynchMesgInit != RK_TRUE))
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }
#endif

    if (mesgPPtr == NULL)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    *mesgPPtr = NULL;

    RK_TCB *fromTaskPtr = NULL;
    RK_ERR err = kMesgResolveFilter_(fromTaskHandle, &fromTaskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (err);
    }

    if ((RK_gRunPtr == NULL) ||
        (RK_gRunPtr->asynchMesgInit != RK_TRUE))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if ((fromTaskPtr != NULL) &&
        (kMesgTasksShareDomain_(RK_gRunPtr, fromTaskPtr) != RK_TRUE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_PARAM);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }

    *mesgPPtr = kMesgDequeueMatching_(RK_gRunPtr, fromTaskPtr);
    if (*mesgPPtr != NULL)
    {
        kTraceRecordObject((*mesgPPtr)->poolPtr, RK_TRACE_OP_RECV,
                           RK_ERR_SUCCESS,
                           RK_gRunPtr->asynchMesgQueue.size);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_SUCCESS);
    }

    if (timeout == RK_NO_WAIT)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_gRunPtr->timeoutNode.waitingQueuePtr =
        &RK_gRunPtr->asynchMesgWaiters;
    if (timeout != RK_WAIT_FOREVER)
    {
        RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
        err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (err);
        }
    }

    RK_gRunPtr->status = RK_RECEIVING;
    RK_gRunPtr->asynchMesgWaitSenderPtr = fromTaskPtr;
    RK_gRunPtr->asynchMesgWaitDestPtr = mesgPPtr;
    RK_gRunPtr->asynchMesgWaitStatus = RK_ERR_SUCCESS;
    err = kTCBQEnq(&RK_gRunPtr->asynchMesgWaiters, RK_gRunPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
    if (err != RK_ERR_SUCCESS)
    {
        if (timeout != RK_WAIT_FOREVER)
        {
            RK_ERR const disarmErr =
                kTimeoutNodeDisarm(&RK_gRunPtr->timeoutNode);
            K_ASSERT(disarmErr == RK_ERR_SUCCESS);
        }
        kMesgClearWait_(RK_gRunPtr);
        RK_gRunPtr->status = RK_RUNNING;
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (err);
    }

    kSyscallTaskSuspend(framePtr, RK_SYSCALL_MESG_WAIT,
                        (ULONG)(UINTPTR)fromTaskHandle,
                        (ULONG)(UINTPTR)mesgPPtr, (ULONG)timeout, 0UL,
                        RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

#endif /* RK_CONF_ASYNCH_MESG && RK_CONF_MESG_QUEUE */
