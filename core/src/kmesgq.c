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
 *   Bounded copy-based message queues and mailboxes. Queue storage is supplied
 *   by the application, while the kernel object lives in a fixed pool and all
 *   runtime access goes through validated handles.
 *
 * Contracts/invariants:
 *   - ringBuf.nFull is always in the inclusive range [0, ringBuf.maxBuf].
 *   - waitingReceivers stores only RK_RECEIVING tasks with mesgQueueRecvBufPtr
 *     set until they are woken or timed out.
 *   - waitingSenders stores only RK_SENDING tasks blocked by a full queue.
 *   - broadcastReceivers reserves the single mailbox slot until every prepared
 *     broadcast receiver has consumed the copy.
 */

#define RK_SOURCE_CODE

#include <kdynobjs.h>
#include <kmesgq.h>
#include <klist.h>
#include <kringbuf.h>
#include <kstring.h>
#include <kapi.h>
#include <ksch.h>
#include <ksystasks.h>
#include <ksyscall.h>
#include <ktrace.h>

#if (RK_CONF_MESG_QUEUE == ON)
/* Convert the public queue/mailbox handle into a checked kernel object pointer. */
static RK_ERR kMesgQueueResolve_(RK_MESG_QUEUE_HANDLE const queueHandle,
                                 RK_MESG_QUEUE **const queuePPtr)
{
    if (queuePPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    *queuePPtr = NULL;

    VOID *objPtr = NULL;
    RK_ERR const err =
        kDynObjResolveHandle(RK_DYN_OBJ_TYPE_MESG_QUEUE, queueHandle,
                             &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
    *queuePPtr = (RK_MESG_QUEUE *)objPtr;
    return (RK_ERR_SUCCESS);
}

static RK_ERR kMesgQueueReadyErr_(RK_MESG_QUEUE const *const kobj)
{
    RK_ERR const readyErr =
        kObjHeaderReadyErr((kobj != NULL) ? &kobj->header : NULL,
                           RK_MESGQQUEUE_KOBJ_ID);
    if (readyErr != RK_ERR_SUCCESS)
    {
        return (readyErr);
    }

    return (kObjHeaderModuleLocalAccessErr(
        &kobj->header, (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr : NULL));
}

static RK_ERR kMesgQueueReportErr_(RK_ERR const err)
{
#if (RK_CONF_ERR_CHECK == ON)
    if (err == RK_ERR_OBJ_NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
    }
    else if (err == RK_ERR_INVALID_OBJ)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
    }
    else if (err == RK_ERR_OBJ_NOT_INIT)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
    }
    else if (err == RK_ERR_INVALID_ISR_PRIMITIVE)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
    }
    else if ((err == RK_ERR_INVALID_MSG_SIZE) ||
             (err == RK_ERR_INVALID_DEPTH) ||
             (err == RK_ERR_INVALID_PARAM))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
    }
#else
    (VOID)err;
#endif
    return (err);
}

RK_ERR kMesgQueueInit(RK_MESG_QUEUE *const kobj, VOID *const bufPtr,
                      const ULONG mesgWords, ULONG const nMesg)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MESG_QUEUE_INIT,
                                        (ULONG)(UINTPTR)kobj,
                                        (ULONG)(UINTPTR)bufPtr,
                                        (ULONG)mesgWords, (ULONG)nMesg));
    }

    RK_ERR const phaseErr = kKernelRawInitGuard();
    if (phaseErr != RK_ERR_SUCCESS)
    {
        return (phaseErr);
    }

    RK_CR_AREA

    RK_CR_ENTER

    if ((kobj == NULL) || (bufPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if ((mesgWords != 1UL) && (mesgWords != 2UL) &&
        (mesgWords != 4UL) && (mesgWords != 8UL) &&
        (mesgWords != 16UL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_MSG_SIZE);
    }

    if (nMesg == 0UL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_DEPTH);
    }

    if (kobj->init == 1)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_DOUBLE_INIT);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_DOUBLE_INIT);
    }

    RK_ERR err = kRingBufInit(&kobj->ringBuf, bufPtr, mesgWords, nMesg);
    K_ASSERT(err == RK_ERR_SUCCESS);

    err = kListInit(&kobj->waitingReceivers);
    if (err != 0)
    {
        RK_CR_EXIT
        return (err);
    }
    err = kListInit(&kobj->waitingSenders);
    if (err != 0)
    {
        RK_CR_EXIT
        return (err);
    }
    kobj->init = 1;
    kobj->objID = RK_MESGQQUEUE_KOBJ_ID;
    kobj->objName[0] = '\0';
    kObjHeaderOwnerModuleSet(&kobj->header,
                             (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr
                                                  : NULL);
    kobj->broadcastReceivers = 0UL;

    kTraceRegisterObject(kobj, RK_MESGQQUEUE_KOBJ_ID);

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)

    kobj->sendNotifyCbk = NULL;

#endif

    RK_CR_EXIT

    return (err);
}

/*
 * Ready a receiver without switching immediately and remember the highest
 * priority task seen. Multi-wake paths use this to perform one reschedule after
 * all queue invariants have been restored.
 */
static VOID kMesgQueueReadyTopTask_(RK_TCB **const chosenTCBPtr,
                                    RK_TCB *const taskPtr)
{
    if ((chosenTCBPtr == NULL) || (taskPtr == NULL))
    {
        return;
    }

    kReadyNoSwtch(taskPtr);
    if ((*chosenTCBPtr == NULL) ||
        (taskPtr->priority < (*chosenTCBPtr)->priority))
    {
        *chosenTCBPtr = taskPtr;
    }
}

/* Remove any armed timeout metadata before a queued task is made ready. */
static VOID kMesgQueueClearBlockingTimeout_(RK_TCB *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return;
    }

    if (taskPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
    {
        kRemoveTimeoutNode(&taskPtr->timeoutNode);
        taskPtr->timeoutNode.timeoutType = 0;
        taskPtr->timeoutNode.waitingQueuePtr = NULL;
    }
}

/*
 * Pop the first normal receiver while leaving broadcast waiters queued. Normal
 * receive and broadcast receive share the same wait list, so waitInfo is the
 * discriminator that keeps their delivery protocols separate.
 */
static RK_ERR kMesgQueueDeqNormalReceiver_(RK_MESG_QUEUE *const kobj,
                                           RK_TCB **const recvTaskPPtr)
{
    RK_NODE *nodePtr = NULL;

    if ((kobj == NULL) || (recvTaskPPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    nodePtr = kobj->waitingReceivers.listDummy.nextPtr;
    while (nodePtr != &kobj->waitingReceivers.listDummy)
    {
        RK_TCB *taskPtr = K_GET_TCB_ADDR(nodePtr);
        if (taskPtr->timeoutNode.waitInfo != RK_MESGQ_RECV_WAIT_BROADCAST)
        {
            RK_ERR err = kListRemove(&kobj->waitingReceivers,
                                     &taskPtr->tcbNode);
            if (err == RK_ERR_SUCCESS)
            {
                taskPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
                *recvTaskPPtr = taskPtr;
            }
            return (err);
        }
        nodePtr = nodePtr->nextPtr;
    }

    *recvTaskPPtr = NULL;
    return (RK_ERR_EMPTY_WAITING_QUEUE);
}

/* Count only receivers that are waiting for a broadcast mailbox delivery. */
static ULONG kMesgQueueCountBroadcastWaiters_(RK_MESG_QUEUE const *const kobj)
{
    ULONG nWaiters = 0UL;
    RK_NODE const *nodePtr = NULL;

    if (kobj == NULL)
    {
        return (0UL);
    }

    nodePtr = kobj->waitingReceivers.listDummy.nextPtr;
    while (nodePtr != &kobj->waitingReceivers.listDummy)
    {
        RK_TCB const *taskPtr = K_GET_TCB_ADDR(nodePtr);
        if (taskPtr->timeoutNode.waitInfo == RK_MESGQ_RECV_WAIT_BROADCAST)
        {
            nWaiters++;
        }
        nodePtr = nodePtr->nextPtr;
    }

    return (nWaiters);
}

/*
 * Mark the receivers that will take part in one broadcast. After this step,
 * broadcastReceivers must match the number of prepared receivers and the
 * mailbox slot must not be reused until they have all drained it.
 */
static UINT kMesgQueuePrepareBroadcastReceivers_(RK_MESG_QUEUE *const kobj,
                                                 UINT const nTasks)
{
    UINT marked = 0U;
    RK_NODE *nodePtr = kobj->waitingReceivers.listDummy.nextPtr;

    while ((marked < nTasks) && (nodePtr != &kobj->waitingReceivers.listDummy))
    {
        RK_NODE *const nextPtr = nodePtr->nextPtr;
        RK_TCB *const recvTaskPtr = K_GET_TCB_ADDR(nodePtr);

        if (recvTaskPtr->timeoutNode.waitInfo == RK_MESGQ_RECV_WAIT_BROADCAST)
        {
            kMesgQueueClearBlockingTimeout_(recvTaskPtr);
            recvTaskPtr->timeoutNode.waitInfo =
                RK_MESGQ_RECV_BROADCAST_DELIVER;
            marked++;
        }
        nodePtr = nextPtr;
    }

    return (marked);
}

RK_ERR kMesgQueueBroadcastWake(RK_MESG_QUEUE *const kobj,
                               UINT const nTasks)
{
    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    UINT woken = 0U;
    RK_TCB *chosenTCBPtr = NULL;
    RK_NODE *nodePtr = kobj->waitingReceivers.listDummy.nextPtr;

    while ((woken < nTasks) && (nodePtr != &kobj->waitingReceivers.listDummy))
    {
        RK_NODE *const nextPtr = nodePtr->nextPtr;
        RK_TCB *const recvTaskPtr = K_GET_TCB_ADDR(nodePtr);

        if (recvTaskPtr->timeoutNode.waitInfo ==
            RK_MESGQ_RECV_BROADCAST_DELIVER)
        {
            RK_ERR err = kListRemove(&kobj->waitingReceivers,
                                     &recvTaskPtr->tcbNode);
            K_ASSERT(err == RK_ERR_SUCCESS);
            kMesgQueueReadyTopTask_(&chosenTCBPtr, recvTaskPtr);
            woken++;
        }
        nodePtr = nextPtr;
    }

    kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, RK_ERR_SUCCESS,
                       kobj->waitingReceivers.size);
    if (chosenTCBPtr != NULL)
    {
        kReschedTask(chosenTCBPtr);
    }

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

static VOID kMesgQueueWakeNormalReceiverIfAny_(RK_MESG_QUEUE *const kobj)
{
    RK_TCB *freeTaskPtr = NULL;

    if (kMesgQueueDeqNormalReceiver_(kobj, &freeTaskPtr) != RK_ERR_SUCCESS)
    {
        return;
    }

    kMesgQueueClearBlockingTimeout_(freeTaskPtr);
    freeTaskPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
    kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, RK_ERR_SUCCESS,
                       kobj->waitingReceivers.size);
    kReadySwtch(freeTaskPtr);
}

static inline VOID kMesgQueueCopy_(RK_MESG_QUEUE const *const kobj,
                                   VOID *const recvPtr,
                                   VOID const *const sendPtr)
{
    ULONG const nBytes = kobj->ringBuf.dataSize * RK_WORD_SIZE;
    RK_MEMCPY(recvPtr, sendPtr, nBytes);
}

static RK_ERR kMesgQueueUserReadValid_(RK_MESG_QUEUE const *const kobj,
                                       VOID const *const ptr)
{
    ULONG bytes;

    if ((kobj == NULL) || (ptr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->ringBuf.dataSize > (RK_ULONG_MAX / (ULONG)RK_WORD_SIZE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    bytes = kobj->ringBuf.dataSize * (ULONG)RK_WORD_SIZE;
    if ((RK_gSyscallThreadModeActive != 0U) &&
        (kMpuUserReadValid(RK_gRunPtr, ptr, bytes) != RK_TRUE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    return (RK_ERR_SUCCESS);
}

static RK_ERR kMesgQueueUserWriteValid_(RK_MESG_QUEUE const *const kobj,
                                        VOID *const ptr)
{
    ULONG bytes;

    if ((kobj == NULL) || (ptr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->ringBuf.dataSize > (RK_ULONG_MAX / (ULONG)RK_WORD_SIZE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    bytes = kobj->ringBuf.dataSize * (ULONG)RK_WORD_SIZE;
    if ((RK_gSyscallThreadModeActive != 0U) &&
        (kMpuUserWriteValid(RK_gRunPtr, ptr, bytes) != RK_TRUE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    return (RK_ERR_SUCCESS);
}

/*
 * Direct-deliver to a blocked normal receiver when the queue has no buffered
 * data and no active broadcast. This preserves FIFO semantics by bypassing the
 * ring only when there is no older message to receive first.
 */
static RK_BOOL kMesgQueueDirectSendIfAny_(RK_MESG_QUEUE *const kobj,
                                          VOID const *const sendPtr,
                                          RK_TRACE_OP const traceOp,
                                          RK_BOOL const notifySend)
{
    RK_TCB *recvTaskPtr = NULL;

    if ((kobj->ringBuf.nFull != 0UL) || (kobj->broadcastReceivers > 0UL))
    {
        return (RK_FALSE);
    }

    if (kMesgQueueDeqNormalReceiver_(kobj, &recvTaskPtr) != RK_ERR_SUCCESS)
    {
        return (RK_FALSE);
    }

    K_ASSERT(recvTaskPtr != NULL);
    K_ASSERT(recvTaskPtr->status == RK_RECEIVING);
    K_ASSERT(recvTaskPtr->mesgQueueRecvBufPtr != NULL);

    kMesgQueueCopy_(kobj, recvTaskPtr->mesgQueueRecvBufPtr, sendPtr);
    recvTaskPtr->mesgQueueRecvBufPtr = NULL;
    recvTaskPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_DIRECT_DELIVER;
    kMesgQueueClearBlockingTimeout_(recvTaskPtr);
    kTraceRecordObject(kobj, traceOp, RK_ERR_SUCCESS, kobj->ringBuf.nFull);

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)
    if ((notifySend == RK_TRUE) && (kobj->sendNotifyCbk != NULL))
    {
        kobj->sendNotifyCbk(kobj);
    }
#else
    (VOID)notifySend;
#endif

    kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, RK_ERR_SUCCESS,
                       kobj->waitingReceivers.size);
    kReadySwtch(recvTaskPtr);
    return (RK_TRUE);
}

/* Wake one blocked sender after a receive frees queue capacity. */
static VOID kMesgQueueWakeSenderIfAny_(RK_MESG_QUEUE *const kobj)
{
    if ((kobj == NULL) || (kobj->waitingSenders.size == 0UL))
    {
        return;
    }

    RK_TCB *freeTaskPtr = NULL;
    freeTaskPtr = kTCBQPeek(&kobj->waitingSenders);
    kTCBQDeq(&kobj->waitingSenders, &freeTaskPtr);
    kMesgQueueClearBlockingTimeout_(freeTaskPtr);
    kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, RK_ERR_SUCCESS,
                       kobj->waitingSenders.size);
    kReadySwtch(freeTaskPtr);
}

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)

RK_ERR kMesgQueueInstallSendCbk(RK_MESG_QUEUE_HANDLE const queueHandle,
                                VOID (*cbk)(RK_MESG_QUEUE *))

{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_INSTALL_SEND_CBK,
            (ULONG)(UINTPTR)queueHandle, (ULONG)(UINTPTR)cbk, 0UL, 0UL));
    }

    RK_ERR const phaseErr = kKernelRawInitGuard();
    if (phaseErr != RK_ERR_SUCCESS)
    {
        return (phaseErr);
    }

    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    kobj->sendNotifyCbk = cbk;
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}
#endif

RK_ERR kMesgQueueSend(RK_MESG_QUEUE_HANDLE const queueHandle,
                      VOID *const sendPtr,
                      const RK_TICK timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_SEND, (ULONG)(UINTPTR)queueHandle,
            (ULONG)(UINTPTR)sendPtr, (ULONG)timeout, 0UL));
    }

    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (K_BLOCKING_ON_ISR(timeout))
    {

        RK_CR_EXIT
        return (kMesgQueueReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (sendPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->ringBuf.nFull >= kobj->ringBuf.maxBuf)
    { /* Queue full */
        if (timeout == 0)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_SEND, RK_ERR_BUFFER_FULL,
                               kobj->ringBuf.nFull);
            RK_CR_EXIT
            return (RK_ERR_BUFFER_FULL);
        }

        do
        {
            if ((timeout != RK_WAIT_FOREVER) && (timeout > 0))
            {
                RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = &kobj->waitingSenders;
                RK_BARRIER
                RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
                if (err != RK_ERR_SUCCESS)
                {
                    RK_gRunPtr->timeoutNode.timeoutType = 0;
                    RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                    kTraceRecordObject(kobj, RK_TRACE_OP_SEND, err,
                                       kobj->waitingSenders.size);
                    RK_CR_EXIT
                    return (err);
                }
            }
            RK_gRunPtr->status = RK_SENDING;
            kTraceRecordObject(kobj, RK_TRACE_OP_SEND_BLOCK, RK_ERR_SUCCESS,
                               kobj->waitingSenders.size + 1UL);
            kTCBQEnqByPrio(&kobj->waitingSenders, RK_gRunPtr);

            kPendCtxSwtch();
            RK_CR_EXIT
            RK_CR_ENTER
            if (RK_gRunPtr->timeOut)
            {
                RK_gRunPtr->timeOut = RK_FALSE;
                kTraceRecordObject(kobj, RK_TRACE_OP_TIMEOUT, RK_ERR_TIMEOUT,
                                   kobj->waitingSenders.size);
                RK_CR_EXIT
                return (RK_ERR_TIMEOUT);
            }
            if ((timeout != RK_WAIT_FOREVER) && (timeout > 0) &&
                (RK_gRunPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING))
            {
                kRemoveTimeoutNode(&RK_gRunPtr->timeoutNode);
                RK_gRunPtr->timeoutNode.timeoutType = 0;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
            }
        } while (kobj->ringBuf.nFull >= kobj->ringBuf.maxBuf);
    }

    if (kMesgQueueDirectSendIfAny_(kobj, sendPtr, RK_TRACE_OP_SEND,
                                   RK_TRUE) == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    kRingBufWrite(&kobj->ringBuf, (ULONG const *)sendPtr);
    kTraceRecordObject(kobj, RK_TRACE_OP_SEND, RK_ERR_SUCCESS,
                       kobj->ringBuf.nFull);

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)
    if (kobj->sendNotifyCbk)
        kobj->sendNotifyCbk(kobj);
#endif
    K_ASSERT(kobj->ringBuf.nFull <= kobj->ringBuf.maxBuf);
    /* unblock a normal reader, if any */
    kMesgQueueWakeNormalReceiverIfAny_(kobj);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgQueueSendSyscall(RK_EXCEPTION_FRAME *const framePtr,
                             RK_MESG_QUEUE_HANDLE const queueHandle,
                             VOID *const sendPtr,
                             RK_TICK const timeout)
{
    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        kSyscallTaskClear(RK_gRunPtr);
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (readyErr);
    }

    if (sendPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR const userErr = kMesgQueueUserReadValid_(kobj, sendPtr);
    if (userErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userErr);
    }

    if (kobj->ringBuf.nFull >= kobj->ringBuf.maxBuf)
    {
        if (timeout == RK_NO_WAIT)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_SEND, RK_ERR_BUFFER_FULL,
                               kobj->ringBuf.nFull);
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (RK_ERR_BUFFER_FULL);
        }

        if ((timeout != RK_WAIT_FOREVER) && (timeout > 0UL))
        {
            RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
            RK_gRunPtr->timeoutNode.waitingQueuePtr = &kobj->waitingSenders;
            RK_BARRIER
            RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                RK_gRunPtr->timeoutNode.timeoutType = 0U;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                kTraceRecordObject(kobj, RK_TRACE_OP_SEND, err,
                                   kobj->waitingSenders.size);
                RK_CR_EXIT
                kSyscallTaskClear(RK_gRunPtr);
                return (err);
            }
        }

        RK_gRunPtr->status = RK_SENDING;
        kTraceRecordObject(kobj, RK_TRACE_OP_SEND_BLOCK, RK_ERR_SUCCESS,
                           kobj->waitingSenders.size + 1UL);
        kTCBQEnqByPrio(&kobj->waitingSenders, RK_gRunPtr);
        kSyscallTaskSuspend(framePtr, RK_SYSCALL_MESG_QUEUE_SEND,
                            (ULONG)(UINTPTR)queueHandle,
                            (ULONG)(UINTPTR)sendPtr, (ULONG)timeout, 0UL,
                            RK_SYSCALL_PHASE_WAIT);
        kPendCtxSwtch();
        RK_CR_EXIT
        return (RK_ERR_SYSCALL_RESTART);
    }

    if (kMesgQueueDirectSendIfAny_(kobj, sendPtr, RK_TRACE_OP_SEND,
                                   RK_TRUE) == RK_TRUE)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_SUCCESS);
    }

    kRingBufWrite(&kobj->ringBuf, (ULONG const *)sendPtr);
    kTraceRecordObject(kobj, RK_TRACE_OP_SEND, RK_ERR_SUCCESS,
                       kobj->ringBuf.nFull);

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)
    if (kobj->sendNotifyCbk)
    {
        kobj->sendNotifyCbk(kobj);
    }
#endif

    K_ASSERT(kobj->ringBuf.nFull <= kobj->ringBuf.maxBuf);
    kMesgQueueWakeNormalReceiverIfAny_(kobj);
    RK_CR_EXIT
    kSyscallTaskClear(RK_gRunPtr);
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgQueueRecv(RK_MESG_QUEUE_HANDLE const queueHandle,
                      VOID *const recvPtr,
                      const RK_TICK timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_RECV, (ULONG)(UINTPTR)queueHandle,
            (ULONG)(UINTPTR)recvPtr, (ULONG)timeout, 0UL));
    }

    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (K_BLOCKING_ON_ISR(timeout))
    {

        RK_CR_EXIT
        return (kMesgQueueReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (recvPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if ((kobj->ringBuf.nFull == 0) || (kobj->broadcastReceivers > 0UL))
    {
        if (timeout == RK_NO_WAIT)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_BUFFER_EMPTY,
                               kobj->ringBuf.nFull);
            RK_CR_EXIT
            return (RK_ERR_BUFFER_EMPTY);
        }
        do
        {
            if ((timeout != RK_WAIT_FOREVER) && (timeout > 0))
            {
                RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
                RK_gRunPtr->timeoutNode.waitingQueuePtr =
                    &kobj->waitingReceivers;
                RK_BARRIER

                RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
                if (err != RK_ERR_SUCCESS)
                {
                    RK_gRunPtr->timeoutNode.timeoutType = 0;
                    RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                    kTraceRecordObject(kobj, RK_TRACE_OP_RECV, err,
                                       kobj->waitingReceivers.size);
                    RK_CR_EXIT
                    return (err);
                }
            }
            RK_gRunPtr->status = RK_RECEIVING;
            RK_gRunPtr->mesgQueueRecvBufPtr = recvPtr;
            RK_gRunPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
            kTraceRecordObject(kobj, RK_TRACE_OP_RECV_BLOCK, RK_ERR_SUCCESS,
                               kobj->waitingReceivers.size + 1UL);
            kTCBQEnqByPrio(&kobj->waitingReceivers, RK_gRunPtr);

            kPendCtxSwtch();

            RK_CR_EXIT
            RK_CR_ENTER
            if (RK_gRunPtr->timeOut)
            {
                RK_gRunPtr->timeOut = RK_FALSE;
                RK_gRunPtr->mesgQueueRecvBufPtr = NULL;
                RK_gRunPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
                kTraceRecordObject(kobj, RK_TRACE_OP_TIMEOUT, RK_ERR_TIMEOUT,
                                   kobj->waitingReceivers.size);
                RK_CR_EXIT
                return (RK_ERR_TIMEOUT);
            }
            if ((timeout != RK_WAIT_FOREVER) && (timeout > 0) &&
                (RK_gRunPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING))
            {
                kRemoveTimeoutNode(&RK_gRunPtr->timeoutNode);
                RK_gRunPtr->timeoutNode.timeoutType = 0;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
            }
            if (RK_gRunPtr->timeoutNode.waitInfo ==
                RK_MESGQ_RECV_DIRECT_DELIVER)
            {
                RK_gRunPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
                RK_gRunPtr->mesgQueueRecvBufPtr = NULL;
                kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_SUCCESS,
                                   kobj->ringBuf.nFull);
                RK_CR_EXIT
                return (RK_ERR_SUCCESS);
            }
        } while ((kobj->ringBuf.nFull == 0) ||
                 (kobj->broadcastReceivers > 0UL));
    }

    kRingBufRead(&kobj->ringBuf, (ULONG *)recvPtr);
    RK_gRunPtr->mesgQueueRecvBufPtr = NULL;
    kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_SUCCESS,
                       kobj->ringBuf.nFull);
    /* Wake a blocked sender now that one queue slot is free. */
    kMesgQueueWakeSenderIfAny_(kobj);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgQueueRecvSyscall(RK_EXCEPTION_FRAME *const framePtr,
                             RK_MESG_QUEUE_HANDLE const queueHandle,
                             VOID *const recvPtr,
                             RK_TICK const timeout)
{
    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        kSyscallTaskClear(RK_gRunPtr);
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (readyErr);
    }

    if (recvPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR const userErr = kMesgQueueUserWriteValid_(kobj, recvPtr);
    if (userErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userErr);
    }

    if ((RK_gRunPtr != NULL) &&
        (RK_gRunPtr->timeoutNode.waitInfo == RK_MESGQ_RECV_DIRECT_DELIVER))
    {
        RK_gRunPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
        RK_gRunPtr->mesgQueueRecvBufPtr = NULL;
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_SUCCESS);
    }

    if ((kobj->ringBuf.nFull == 0UL) || (kobj->broadcastReceivers > 0UL))
    {
        if (timeout == RK_NO_WAIT)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_BUFFER_EMPTY,
                               kobj->ringBuf.nFull);
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (RK_ERR_BUFFER_EMPTY);
        }

        if ((timeout != RK_WAIT_FOREVER) && (timeout > 0UL))
        {
            RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
            RK_gRunPtr->timeoutNode.waitingQueuePtr =
                &kobj->waitingReceivers;
            RK_BARRIER

            RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                RK_gRunPtr->timeoutNode.timeoutType = 0U;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                kTraceRecordObject(kobj, RK_TRACE_OP_RECV, err,
                                   kobj->waitingReceivers.size);
                RK_CR_EXIT
                kSyscallTaskClear(RK_gRunPtr);
                return (err);
            }
        }

        RK_gRunPtr->status = RK_RECEIVING;
        RK_gRunPtr->mesgQueueRecvBufPtr = recvPtr;
        RK_gRunPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
        kTraceRecordObject(kobj, RK_TRACE_OP_RECV_BLOCK, RK_ERR_SUCCESS,
                           kobj->waitingReceivers.size + 1UL);
        kTCBQEnqByPrio(&kobj->waitingReceivers, RK_gRunPtr);
        kSyscallTaskSuspend(framePtr, RK_SYSCALL_MESG_QUEUE_RECV,
                            (ULONG)(UINTPTR)queueHandle,
                            (ULONG)(UINTPTR)recvPtr, (ULONG)timeout, 0UL,
                            RK_SYSCALL_PHASE_WAIT);
        kPendCtxSwtch();
        RK_CR_EXIT
        return (RK_ERR_SYSCALL_RESTART);
    }

    kRingBufRead(&kobj->ringBuf, (ULONG *)recvPtr);
    RK_gRunPtr->mesgQueueRecvBufPtr = NULL;
    kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_SUCCESS,
                       kobj->ringBuf.nFull);
    kMesgQueueWakeSenderIfAny_(kobj);
    RK_CR_EXIT
    kSyscallTaskClear(RK_gRunPtr);
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgQueuePeek(RK_MESG_QUEUE_HANDLE const queueHandle,
                      VOID *const recvPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_PEEK, (ULONG)(UINTPTR)queueHandle,
            (ULONG)(UINTPTR)recvPtr, 0UL, 0UL));
    }

    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (recvPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if ((kobj->ringBuf.nFull == 0) || (kobj->broadcastReceivers > 0UL))
    {
        RK_CR_EXIT
        return (RK_ERR_BUFFER_EMPTY);
    }

    if (RK_gSyscallThreadModeActive != 0U)
    {
        RK_ERR const userErr = kMesgQueueUserWriteValid_(kobj, recvPtr);
        if (userErr != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (userErr);
        }
    }

    kRingBufPeek(&kobj->ringBuf, (ULONG *)recvPtr);

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgQueueJam(RK_MESG_QUEUE_HANDLE const queueHandle,
                     VOID *const sendPtr,
                     const RK_TICK timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_JAM, (ULONG)(UINTPTR)queueHandle,
            (ULONG)(UINTPTR)sendPtr, (ULONG)timeout, 0UL));
    }

    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (K_BLOCKING_ON_ISR(timeout))
    {

        RK_CR_EXIT
        return (kMesgQueueReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (sendPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->ringBuf.nFull >= kobj->ringBuf.maxBuf)
    { /* Queue full */
        if (timeout == RK_NO_WAIT)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_JAM, RK_ERR_BUFFER_FULL,
                               kobj->ringBuf.nFull);
            RK_CR_EXIT
            return (RK_ERR_BUFFER_FULL);
        }

        do
        {
            if ((timeout != RK_WAIT_FOREVER) && (timeout > 0))
            {
                RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = &kobj->waitingSenders;
                RK_BARRIER

                RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
                if (err != RK_ERR_SUCCESS)
                {
                    RK_gRunPtr->timeoutNode.timeoutType = 0;
                    RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                    kTraceRecordObject(kobj, RK_TRACE_OP_JAM, err,
                                       kobj->waitingSenders.size);
                    RK_CR_EXIT
                    return (err);
                }
            }
            RK_gRunPtr->status = RK_SENDING;
            kTraceRecordObject(kobj, RK_TRACE_OP_JAM_BLOCK, RK_ERR_SUCCESS,
                               kobj->waitingSenders.size + 1UL);

            kTCBQEnqByPrio(&kobj->waitingSenders, RK_gRunPtr);

            kPendCtxSwtch();
            RK_CR_EXIT
            RK_CR_ENTER
            if (RK_gRunPtr->timeOut)
            {
                RK_gRunPtr->timeOut = RK_FALSE;
                kTraceRecordObject(kobj, RK_TRACE_OP_TIMEOUT, RK_ERR_TIMEOUT,
                                   kobj->waitingSenders.size);
                RK_CR_EXIT
                return (RK_ERR_TIMEOUT);
            }
            if ((timeout != RK_WAIT_FOREVER) && (timeout > 0) &&
                (RK_gRunPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING))
            {
                kRemoveTimeoutNode(&RK_gRunPtr->timeoutNode);
                RK_gRunPtr->timeoutNode.timeoutType = 0;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
            }
        } while (kobj->ringBuf.nFull >= kobj->ringBuf.maxBuf);
    }

    if (kMesgQueueDirectSendIfAny_(kobj, sendPtr, RK_TRACE_OP_JAM,
                                   RK_TRUE) == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    kRingBufJam(&kobj->ringBuf, (ULONG const *)sendPtr);
    kTraceRecordObject(kobj, RK_TRACE_OP_JAM, RK_ERR_SUCCESS,
                       kobj->ringBuf.nFull);

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)

    if (kobj->sendNotifyCbk)
        kobj->sendNotifyCbk(kobj);

#endif

    /* Wake a normal receiver if this write made data available. */
    kMesgQueueWakeNormalReceiverIfAny_(kobj);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgQueueJamSyscall(RK_EXCEPTION_FRAME *const framePtr,
                            RK_MESG_QUEUE_HANDLE const queueHandle,
                            VOID *const sendPtr,
                            RK_TICK const timeout)
{
    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        kSyscallTaskClear(RK_gRunPtr);
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (readyErr);
    }

    if (sendPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR const userErr = kMesgQueueUserReadValid_(kobj, sendPtr);
    if (userErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userErr);
    }

    if (kobj->ringBuf.nFull >= kobj->ringBuf.maxBuf)
    {
        if (timeout == RK_NO_WAIT)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_JAM, RK_ERR_BUFFER_FULL,
                               kobj->ringBuf.nFull);
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (RK_ERR_BUFFER_FULL);
        }

        if ((timeout != RK_WAIT_FOREVER) && (timeout > 0UL))
        {
            RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
            RK_gRunPtr->timeoutNode.waitingQueuePtr = &kobj->waitingSenders;
            RK_BARRIER

            RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                RK_gRunPtr->timeoutNode.timeoutType = 0U;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                kTraceRecordObject(kobj, RK_TRACE_OP_JAM, err,
                                   kobj->waitingSenders.size);
                RK_CR_EXIT
                kSyscallTaskClear(RK_gRunPtr);
                return (err);
            }
        }

        RK_gRunPtr->status = RK_SENDING;
        kTraceRecordObject(kobj, RK_TRACE_OP_JAM_BLOCK, RK_ERR_SUCCESS,
                           kobj->waitingSenders.size + 1UL);
        kTCBQEnqByPrio(&kobj->waitingSenders, RK_gRunPtr);
        kSyscallTaskSuspend(framePtr, RK_SYSCALL_MESG_QUEUE_JAM,
                            (ULONG)(UINTPTR)queueHandle,
                            (ULONG)(UINTPTR)sendPtr, (ULONG)timeout, 0UL,
                            RK_SYSCALL_PHASE_WAIT);
        kPendCtxSwtch();
        RK_CR_EXIT
        return (RK_ERR_SYSCALL_RESTART);
    }

    if (kMesgQueueDirectSendIfAny_(kobj, sendPtr, RK_TRACE_OP_JAM,
                                   RK_TRUE) == RK_TRUE)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_SUCCESS);
    }

    kRingBufJam(&kobj->ringBuf, (ULONG const *)sendPtr);
    kTraceRecordObject(kobj, RK_TRACE_OP_JAM, RK_ERR_SUCCESS,
                       kobj->ringBuf.nFull);

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)
    if (kobj->sendNotifyCbk)
    {
        kobj->sendNotifyCbk(kobj);
    }
#endif

    kMesgQueueWakeNormalReceiverIfAny_(kobj);
    RK_CR_EXIT
    kSyscallTaskClear(RK_gRunPtr);
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgQueueQuery(RK_MESG_QUEUE_HANDLE const queueHandle,
                       UINT *const nMesgPtr, UINT *const nWaitRPtr,
                       UINT *const nWaitSPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_QUERY, (ULONG)(UINTPTR)queueHandle,
            (ULONG)(UINTPTR)nMesgPtr, (ULONG)(UINTPTR)nWaitRPtr,
            (ULONG)(UINTPTR)nWaitSPtr));
    }

    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if ((nMesgPtr == NULL) && (nWaitRPtr == NULL) && (nWaitSPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    if (nMesgPtr != NULL)
    {
        *nMesgPtr = (UINT)kobj->ringBuf.nFull;
    }
    if (nWaitRPtr != NULL)
    {
        *nWaitRPtr = (UINT)kobj->waitingReceivers.size;
    }
    if (nWaitSPtr != NULL)
    {
        *nWaitSPtr = (UINT)kobj->waitingSenders.size;
    }

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgQueueReset(RK_MESG_QUEUE_HANDLE const queueHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_RESET, (ULONG)(UINTPTR)queueHandle, 0UL,
            0UL, 0UL));
    }

    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER
    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    UINT toWakeR = kobj->waitingReceivers.size;
    UINT toWakeS = kobj->waitingSenders.size;
    UINT toWake = toWakeR + toWakeS;
    /* Defer only when running in ISR context; handle multi-wake inline
       otherwise to avoid re-enqueue loops when the PostProc worker invokes this
       helper. */
    if ((toWake > 0U) && kIsISR())
    {
        RK_CR_EXIT
        return (
            kPostProcJobEnq(RK_POSTPROC_JOB_MESGQ_RESET, (VOID *)kobj, toWake));
    }

    kRingBufReset(&kobj->ringBuf);
    kobj->broadcastReceivers = 0UL;
    kTraceRecordObject(kobj, RK_TRACE_OP_RESET, RK_ERR_SUCCESS, toWake);

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)

    kobj->sendNotifyCbk = NULL;
#endif

    if (toWake == 0U)
    {
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    RK_TCB *chosenTCBPtr = NULL;
    for (UINT i = 0U; i < toWakeR; i++)
    {
        RK_TCB *nextTCBPtr = NULL;
        kTCBQDeq(&kobj->waitingReceivers, &nextTCBPtr);
        if (nextTCBPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
        {
            kRemoveTimeoutNode(&nextTCBPtr->timeoutNode);
            nextTCBPtr->timeoutNode.timeoutType = 0;
            nextTCBPtr->timeoutNode.waitingQueuePtr = NULL;
        }
        nextTCBPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
        nextTCBPtr->mesgQueueRecvBufPtr = NULL;
        kReadyNoSwtch(nextTCBPtr);
        if ((chosenTCBPtr == NULL) ||
            (nextTCBPtr->priority < chosenTCBPtr->priority))
        {
            chosenTCBPtr = nextTCBPtr;
        }
    }
    for (UINT i = 0U; i < toWakeS; i++)
    {
        RK_TCB *nextTCBPtr = NULL;
        kTCBQDeq(&kobj->waitingSenders, &nextTCBPtr);
        if (nextTCBPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
        {
            kRemoveTimeoutNode(&nextTCBPtr->timeoutNode);
            nextTCBPtr->timeoutNode.timeoutType = 0;
            nextTCBPtr->timeoutNode.waitingQueuePtr = NULL;
        }
        kReadyNoSwtch(nextTCBPtr);
        if ((chosenTCBPtr == NULL) ||
            (nextTCBPtr->priority < chosenTCBPtr->priority))
        {
            chosenTCBPtr = nextTCBPtr;
        }
    }
    if (chosenTCBPtr != NULL)
    {
        kReschedTask(chosenTCBPtr);
    }
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

/* this works only for N=1 */
RK_ERR kMesgQueuePostOvw(RK_MESG_QUEUE_HANDLE const queueHandle,
                         VOID *sendPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_POST_OVW, (ULONG)(UINTPTR)queueHandle,
            (ULONG)(UINTPTR)sendPtr, 0UL, 0UL));
    }

    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (sendPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->ringBuf.maxBuf > 1)
    {
        RK_CR_EXIT
        return (RK_ERR_MESGQ_NOT_A_MBOX);
    }

    if (kobj->broadcastReceivers > 0UL)
    {
        RK_CR_EXIT
        return (RK_ERR_BUFFER_FULL);
    }

    if (RK_gSyscallThreadModeActive != 0U)
    {
        RK_ERR const userErr = kMesgQueueUserReadValid_(kobj, sendPtr);
        if (userErr != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (userErr);
        }
    }

    if (kMesgQueueDirectSendIfAny_(kobj, sendPtr, RK_TRACE_OP_SEND,
                                   RK_FALSE) == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    RK_BOOL wasEmpty = RK_FALSE;

    if (kobj->ringBuf.nFull == 0)
    {
        wasEmpty = RK_TRUE;
    }

    kRingBufOverwrite(&kobj->ringBuf, (ULONG const *)sendPtr);
    kTraceRecordObject(kobj, RK_TRACE_OP_SEND, RK_ERR_SUCCESS,
                       kobj->ringBuf.nFull);

    if (wasEmpty)
    {
        kMesgQueueWakeNormalReceiverIfAny_(kobj);
    }

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

/* Broadcast is defined only for one-slot mailboxes. */
RK_ERR kMesgQueueBroadcast(RK_MESG_QUEUE_HANDLE const queueHandle,
                           VOID *const sendPtr, UINT *const nRecvPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_BROADCAST, (ULONG)(UINTPTR)queueHandle,
            (ULONG)(UINTPTR)sendPtr, (ULONG)(UINTPTR)nRecvPtr, 0UL));
    }

    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    if (nRecvPtr != NULL)
    {
        *nRecvPtr = 0U;
    }

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (sendPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (RK_gSyscallThreadModeActive != 0U)
    {
        RK_ERR const userErr = kMesgQueueUserReadValid_(kobj, sendPtr);
        if (userErr != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (userErr);
        }
    }

    if (kobj->ringBuf.maxBuf > 1)
    {
        RK_CR_EXIT
        return (RK_ERR_MESGQ_NOT_A_MBOX);
    }

    UINT const toWake = (UINT)kMesgQueueCountBroadcastWaiters_(kobj);

    if (toWake == 0U)
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_SEND, RK_ERR_BUFFER_EMPTY,
                           kobj->ringBuf.nFull);
        RK_CR_EXIT
        return (RK_ERR_BUFFER_EMPTY);
    }

    if (kobj->ringBuf.nFull >= kobj->ringBuf.maxBuf)
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_SEND, RK_ERR_BUFFER_FULL,
                           kobj->ringBuf.nFull);
        RK_CR_EXIT
        return (RK_ERR_BUFFER_FULL);
    }

    RK_BOOL const deferWake = (toWake > 1U) ? RK_TRUE : RK_FALSE;
    if (deferWake == RK_TRUE)
    {
        RK_ERR const deferErr =
            kPostProcJobEnq(RK_POSTPROC_JOB_MESGQ_BROADCAST_WAKE,
                            (VOID *)kobj, toWake);
        if (deferErr != RK_ERR_SUCCESS)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, deferErr,
                               (ULONG)toWake);
            RK_CR_EXIT
            return (deferErr);
        }
    }

    kobj->broadcastReceivers = (ULONG)toWake;
    kRingBufWrite(&kobj->ringBuf, (ULONG const *)sendPtr);
    kTraceRecordObject(kobj, RK_TRACE_OP_SEND, RK_ERR_SUCCESS,
                       kobj->ringBuf.nFull);

    UINT const prepared = kMesgQueuePrepareBroadcastReceivers_(kobj, toWake);
    K_ASSERT(prepared == toWake);
    kobj->broadcastReceivers = (ULONG)prepared;

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)
    if (kobj->sendNotifyCbk)
        kobj->sendNotifyCbk(kobj);
#endif

    if (nRecvPtr != NULL)
    {
        *nRecvPtr = prepared;
    }
    if (deferWake == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    RK_ERR const wakeErr = kMesgQueueBroadcastWake(kobj, prepared);
    RK_CR_EXIT
    return (wakeErr);
}

RK_ERR kMesgQueueBroadcastRecv(RK_MESG_QUEUE_HANDLE const queueHandle,
                               VOID *const recvPtr,
                               const RK_TICK timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_BROADCAST_RECV,
            (ULONG)(UINTPTR)queueHandle, (ULONG)(UINTPTR)recvPtr,
            (ULONG)timeout, 0UL));
    }

    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (K_BLOCKING_ON_ISR(timeout))
    {
        RK_CR_EXIT
        return (kMesgQueueReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (recvPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->ringBuf.maxBuf > 1UL)
    {
        RK_CR_EXIT
        return (RK_ERR_MESGQ_NOT_A_MBOX);
    }

    while (RK_gRunPtr->timeoutNode.waitInfo !=
           RK_MESGQ_RECV_BROADCAST_DELIVER)
    {
        if (timeout == RK_NO_WAIT)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_BUFFER_EMPTY,
                               kobj->ringBuf.nFull);
            RK_CR_EXIT
            return (RK_ERR_BUFFER_EMPTY);
        }

        if ((timeout != RK_WAIT_FOREVER) && (timeout > 0))
        {
            RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
            RK_gRunPtr->timeoutNode.waitingQueuePtr =
                &kobj->waitingReceivers;
            RK_BARRIER

            RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                RK_gRunPtr->timeoutNode.timeoutType = 0;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                kTraceRecordObject(kobj, RK_TRACE_OP_RECV, err,
                                   kobj->waitingReceivers.size);
                RK_CR_EXIT
                return (err);
            }
        }
        RK_gRunPtr->status = RK_RECEIVING;
        RK_gRunPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_BROADCAST;
        kTraceRecordObject(kobj, RK_TRACE_OP_RECV_BLOCK, RK_ERR_SUCCESS,
                           kobj->waitingReceivers.size + 1UL);
        kTCBQEnqByPrio(&kobj->waitingReceivers, RK_gRunPtr);

        kPendCtxSwtch();

        RK_CR_EXIT
        RK_CR_ENTER
        if (RK_gRunPtr->timeOut)
        {
            RK_gRunPtr->timeOut = RK_FALSE;
            RK_gRunPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
            kTraceRecordObject(kobj, RK_TRACE_OP_TIMEOUT, RK_ERR_TIMEOUT,
                               kobj->waitingReceivers.size);
            RK_CR_EXIT
            return (RK_ERR_TIMEOUT);
        }
        if ((timeout != RK_WAIT_FOREVER) && (timeout > 0) &&
            (RK_gRunPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING))
        {
            kRemoveTimeoutNode(&RK_gRunPtr->timeoutNode);
            RK_gRunPtr->timeoutNode.timeoutType = 0;
            RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
        }
    }

    K_ASSERT(kobj->ringBuf.nFull == 1UL);
    K_ASSERT(kobj->broadcastReceivers > 0UL);

    RK_gRunPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;

    if (kobj->broadcastReceivers == 1UL)
    {
        kRingBufRead(&kobj->ringBuf, (ULONG *)recvPtr);
        kobj->broadcastReceivers = 0UL;
        kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_SUCCESS,
                           kobj->ringBuf.nFull);
        kMesgQueueWakeSenderIfAny_(kobj);
    }
    else
    {
        kRingBufPeek(&kobj->ringBuf, (ULONG *)recvPtr);
        kobj->broadcastReceivers--;
        kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_SUCCESS,
                           kobj->ringBuf.nFull);
    }

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMesgQueueBroadcastRecvSyscall(RK_EXCEPTION_FRAME *const framePtr,
                                      RK_MESG_QUEUE_HANDLE const queueHandle,
                                      VOID *const recvPtr,
                                      RK_TICK const timeout)
{
    RK_MESG_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kMesgQueueResolve_(queueHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        kSyscallTaskClear(RK_gRunPtr);
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMesgQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kMesgQueueReportErr_(readyErr);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (readyErr);
    }

    if (recvPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR const userErr = kMesgQueueUserWriteValid_(kobj, recvPtr);
    if (userErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userErr);
    }

    if (kobj->ringBuf.maxBuf > 1UL)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_MESGQ_NOT_A_MBOX);
    }

    if ((RK_gRunPtr == NULL) ||
        (RK_gRunPtr->timeoutNode.waitInfo !=
         RK_MESGQ_RECV_BROADCAST_DELIVER))
    {
        if (timeout == RK_NO_WAIT)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_BUFFER_EMPTY,
                               kobj->ringBuf.nFull);
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (RK_ERR_BUFFER_EMPTY);
        }

        if ((timeout != RK_WAIT_FOREVER) && (timeout > 0UL))
        {
            RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
            RK_gRunPtr->timeoutNode.waitingQueuePtr =
                &kobj->waitingReceivers;
            RK_BARRIER

            RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                RK_gRunPtr->timeoutNode.timeoutType = 0U;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                kTraceRecordObject(kobj, RK_TRACE_OP_RECV, err,
                                   kobj->waitingReceivers.size);
                RK_CR_EXIT
                kSyscallTaskClear(RK_gRunPtr);
                return (err);
            }
        }

        RK_gRunPtr->status = RK_RECEIVING;
        RK_gRunPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_BROADCAST;
        kTraceRecordObject(kobj, RK_TRACE_OP_RECV_BLOCK, RK_ERR_SUCCESS,
                           kobj->waitingReceivers.size + 1UL);
        kTCBQEnqByPrio(&kobj->waitingReceivers, RK_gRunPtr);
        kSyscallTaskSuspend(framePtr, RK_SYSCALL_MESG_QUEUE_BROADCAST_RECV,
                            (ULONG)(UINTPTR)queueHandle,
                            (ULONG)(UINTPTR)recvPtr, (ULONG)timeout, 0UL,
                            RK_SYSCALL_PHASE_WAIT);
        kPendCtxSwtch();
        RK_CR_EXIT
        return (RK_ERR_SYSCALL_RESTART);
    }

    K_ASSERT(kobj->ringBuf.nFull == 1UL);
    K_ASSERT(kobj->broadcastReceivers > 0UL);

    RK_gRunPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;

    if (kobj->broadcastReceivers == 1UL)
    {
        kRingBufRead(&kobj->ringBuf, (ULONG *)recvPtr);
        kobj->broadcastReceivers = 0UL;
        kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_SUCCESS,
                           kobj->ringBuf.nFull);
        kMesgQueueWakeSenderIfAny_(kobj);
    }
    else
    {
        kRingBufPeek(&kobj->ringBuf, (ULONG *)recvPtr);
        kobj->broadcastReceivers--;
        kTraceRecordObject(kobj, RK_TRACE_OP_RECV, RK_ERR_SUCCESS,
                           kobj->ringBuf.nFull);
    }

    RK_CR_EXIT
    kSyscallTaskClear(RK_gRunPtr);
    return (RK_ERR_SUCCESS);
}

#endif /* RK_CONF_MESG_QUEUE */
