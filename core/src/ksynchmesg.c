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
 *   Synchronous task messaging. Plain send/receive is a blocking copy
 *   rendezvous. Call/accept/reply is an extended rendezvous with caller/server
 *   state in task control blocks so blocking calls can resume through the
 *   syscall continuation path after a wakeup or timeout.
 *
 * Contracts/invariants:
 *   - A queued sender is linked from exactly one receiver's synchMesgSenders
 *     list and points back to that receiver.
 *   - A pending send cached on the receiver must name the same sender at the
 *     head of the sender queue.
 *   - Queued plain senders can raise receiver priority until consumed.
 *   - Accepted call/reply substitutes caller priority until reply.
 *   - At most one extended call is active per server.
 *   - Timeout and cleanup paths must clear both sides of every TCB link.
 */

#define RK_SOURCE_CODE

#include <ksynchmesg.h>
#include <kapi.h>
#include <kstring.h>
#include <ksyscall.h>
#include <ktrace.h>

#if (RK_CONF_SYNCH_MESG == ON)

static inline VOID kSynchMesgClearSender_(RK_TCB *const senderPtr)
{
    senderPtr->synchMesgPtr = NULL;
    senderPtr->synchMesgBytes = 0UL;
    senderPtr->synchMesgReceiverPtr = NULL;
}

static inline VOID kSynchMesgClearCall_(RK_TCB *const callerPtr)
{
    callerPtr->synchMesgPtr = NULL;
    callerPtr->synchMesgBytes = 0UL;
    callerPtr->synchMesgReceiverPtr = NULL;
    callerPtr->synchMesgStatus = RK_ERR_SUCCESS;
    callerPtr->synchMesgCallReplyBufPtr = NULL;
    callerPtr->synchMesgCallReplyBytesPtr = NULL;
    callerPtr->synchMesgCallReplyMaxBytes = 0UL;
    callerPtr->synchMesgCallState = RK_SYNCH_CALL_IDLE;
}

static inline VOID kSynchMesgClearActiveCall_(RK_TCB *const serverPtr)
{
    serverPtr->synchMesgActiveCallerPtr = NULL;
    serverPtr->synchMesgActiveCallerPrio = serverPtr->prioNominal;
}

static inline VOID kSynchMesgClearReceiverWait_(RK_TCB *const receiverPtr)
{
    receiverPtr->synchMesgRecvBufPtr = NULL;
    receiverPtr->synchMesgRecvBytesPtr = NULL;
}

static inline RK_BOOL kSynchMesgTaskOwnsMutex_(RK_TCB const *const taskPtr)
{
#if (RK_CONF_MUTEX == ON)
    return (((taskPtr != NULL) && (taskPtr->ownedMutexList.size > 0UL))
                ? RK_TRUE
                : RK_FALSE);
#else
    (VOID)taskPtr;
    return (RK_FALSE);
#endif
}

static inline RK_BOOL kSynchMesgBytesValid_(ULONG const mesgBytes)
{
    return (((mesgBytes != 0UL) && ((mesgBytes % RK_WORD_SIZE) == 0UL))
                ? RK_TRUE
                : RK_FALSE);
}

static inline RK_ERR kSynchMesgPublicReadyErr_(RK_ERR const err)
{
    if ((err == RK_ERR_RESCHED_PENDING) ||
        (err == RK_ERR_RESCHED_NOT_NEEDED))
    {
        return (RK_ERR_SUCCESS);
    }

    return (err);
}

static RK_ERR kSynchMesgReportErr_(RK_ERR const err)
{
#if (RK_CONF_ERR_CHECK == ON)
    if (err == RK_ERR_OBJ_NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
    }
    else if (err == RK_ERR_OBJ_NOT_INIT)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
    }
    else if ((err == RK_ERR_INVALID_PARAM) ||
             (err == RK_ERR_INVALID_MSG_SIZE))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
    }
    else if (err == RK_ERR_INVALID_TIMEOUT)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
    }
    else if (err == RK_ERR_INVALID_ISR_PRIMITIVE)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
    }
    else if (err == RK_ERR_TASK_INVALID_ST)
    {
        K_ERR_HANDLER(RK_FAULT_TASK_INVALID_STATE);
    }
    else if (err == RK_ERR_SYNCH_CALL_NOT_ACTIVE)
    {
        K_ERR_HANDLER(RK_FAULT_SYNCH_CALL_NOT_ACTIVE);
    }
#else
    (VOID)err;
#endif
    return (err);
}

static RK_ERR kSynchMesgCurrentTaskErr_(RK_TICK const timeout)
{
    if ((RK_gRunPtr == NULL) || K_BLOCKING_ON_ISR(timeout))
    {
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    return (RK_ERR_SUCCESS);
}

static inline VOID kSynchMesgDisarmTimeout_(RK_TCB *const taskPtr)
{
    if (kTimeoutNodeIsArmed(&taskPtr->timeoutNode) == RK_TRUE)
    {
        RK_ERR err = kTimeoutNodeDisarm(&taskPtr->timeoutNode);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    else
    {
        kTimeoutNodeReset(&taskPtr->timeoutNode);
    }
}

static VOID kSynchMesgUpdateReceiverPrio_(RK_TCB *const receiverPtr)
{
    kTaskUpdateEffectivePrioChain(receiverPtr);
}

static VOID kSynchMesgWakeAcceptor_(RK_TCB *const serverPtr)
{
    if ((serverPtr == NULL) || (serverPtr->synchMesgAcceptWaiters.size == 0UL))
    {
        return;
    }

    RK_TCB *acceptorPtr = kTCBQPeek(&serverPtr->synchMesgAcceptWaiters);
    RK_ERR err = kTCBQDeq(&serverPtr->synchMesgAcceptWaiters, &acceptorPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);

    if (acceptorPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
    {
        kSynchMesgDisarmTimeout_(acceptorPtr);
    }
    kReadySwtch(acceptorPtr);
}

static VOID kSynchMesgCopy_(VOID *const recvPtr,
                             VOID const *const mesgPtr,
                             ULONG const mesgBytes)
{
    RK_MEMCPY(recvPtr, mesgPtr, mesgBytes);
}

static RK_ERR kSynchMesgUserReadValid_(VOID const *const ptr,
                                       ULONG const bytes)
{
    if (ptr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    return ((kMpuUserReadValid(RK_gRunPtr, ptr, bytes) == RK_TRUE)
                ? RK_ERR_SUCCESS
                : RK_ERR_INVALID_PARAM);
}

static RK_ERR kSynchMesgUserWriteValid_(VOID *const ptr,
                                        ULONG const bytes)
{
    if (ptr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    return ((kMpuUserWriteValid(RK_gRunPtr, ptr, bytes) == RK_TRUE)
                ? RK_ERR_SUCCESS
                : RK_ERR_INVALID_PARAM);
}

static RK_ERR kSynchMesgUserWriteOptional_(VOID *const ptr,
                                           ULONG const bytes)
{
    if (ptr == NULL)
    {
        return (RK_ERR_SUCCESS);
    }

    return ((kMpuUserWriteValid(RK_gRunPtr, ptr, bytes) == RK_TRUE)
                ? RK_ERR_SUCCESS
                : RK_ERR_INVALID_PARAM);
}

static inline VOID kSynchMesgRecvBytesSet_(ULONG *const mesgBytesPtr,
                                                ULONG const mesgBytes)
{
    if (mesgBytesPtr != NULL)
    {
        *mesgBytesPtr = mesgBytes;
    }
}

static inline ULONG kSynchMesgSenderBytes_(
    RK_TCB const *const receiverPtr,
    RK_TCB const *const senderPtr)
{
    K_UNUSE(receiverPtr);
    return (senderPtr->synchMesgBytes);
}

static RK_ERR kSynchMesgDirectRecv_(RK_TCB *const receiverPtr,
                                     VOID const *const mesgPtr,
                                     ULONG const mesgBytes)
{
    K_ASSERT(receiverPtr->synchMesgRecvBufPtr != NULL);

    kSynchMesgCopy_(receiverPtr->synchMesgRecvBufPtr, mesgPtr, mesgBytes);
    kSynchMesgRecvBytesSet_(receiverPtr->synchMesgRecvBytesPtr,
                                 mesgBytes);
    receiverPtr->synchMesgRecvStatus = RK_ERR_SUCCESS;

    kSynchMesgClearReceiverWait_(receiverPtr);

    if (receiverPtr->timeoutNode.timeoutType == RK_TIMEOUT_SYNCH_RECV)
    {
        kSynchMesgDisarmTimeout_(receiverPtr);
    }

    kReadySwtch(receiverPtr);
    return (RK_ERR_SUCCESS);
}

static VOID kSynchMesgPromoteNext_(RK_TCB *const receiverPtr)
{
    /*
     * The receiver caches the next sender's payload pointer so receive can
     * consume without scanning. The sender queue remains authoritative.
     */
    if ((receiverPtr == NULL) ||
        (receiverPtr->synchMesgPendingPtr != NULL))
    {
        return;
    }

    if (receiverPtr->synchMesgSenders.size > 0U)
    {
        RK_TCB *senderPtr = kTCBQPeek(&receiverPtr->synchMesgSenders);
        receiverPtr->synchMesgPendingPtr = senderPtr->synchMesgPtr;
        receiverPtr->synchMesgPendingSenderPtr = senderPtr;
    }
}

static RK_ERR kSynchMesgConsumePendingSend_(RK_TCB *const receiverPtr,
                                             VOID *const recvPtr,
                                             ULONG *const mesgBytesPtr)
{
    /*
     * Consume exactly the cached pending sender: copy payload, mark sender
     * successful, unlink it, clear both sides, then promote the next sender if
     * one is queued.
     */
    K_ASSERT(receiverPtr != NULL);
    K_ASSERT(receiverPtr->synchMesgPendingPtr != NULL);

    RK_TCB *senderPtr = receiverPtr->synchMesgPendingSenderPtr;
    K_ASSERT(senderPtr != NULL);
    ULONG const mesgBytes =
        kSynchMesgSenderBytes_(receiverPtr, senderPtr);

    kSynchMesgCopy_(recvPtr, receiverPtr->synchMesgPendingPtr,
                     mesgBytes);
    kSynchMesgRecvBytesSet_(mesgBytesPtr, mesgBytes);
    senderPtr->synchMesgStatus = RK_ERR_SUCCESS;

    receiverPtr->synchMesgPendingPtr = NULL;
    receiverPtr->synchMesgPendingSenderPtr = NULL;

    RK_TCB *remPtr = senderPtr;
    RK_ERR err = kTCBQRem(&receiverPtr->synchMesgSenders, &remPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    if (senderPtr->timeoutNode.timeoutType == RK_TIMEOUT_SYNCH_SEND)
    {
        kSynchMesgDisarmTimeout_(senderPtr);
    }

    kSynchMesgClearSender_(senderPtr);

    kSynchMesgPromoteNext_(receiverPtr);
    kSynchMesgUpdateReceiverPrio_(receiverPtr);

    err = kReadySwtch(senderPtr);
    if (err < 0)
    {
        return (err);
    }

    return (RK_ERR_SUCCESS);
}

VOID kSynchMesgTimeoutSend(RK_TCB *const senderPtr)
{
    RK_TCB *const receiverPtr = senderPtr->synchMesgReceiverPtr;
    if (receiverPtr == NULL)
    {
        return;
    }

    if (receiverPtr->synchMesgPendingSenderPtr == senderPtr)
    {
        receiverPtr->synchMesgPendingPtr = NULL;
        receiverPtr->synchMesgPendingSenderPtr = NULL;
    }

    RK_TCB *remPtr = senderPtr;
    RK_ERR err = kTCBQRem(&receiverPtr->synchMesgSenders, &remPtr);
    K_ASSERT(err == RK_ERR_SUCCESS);

    kSynchMesgClearSender_(senderPtr);
    kSynchMesgPromoteNext_(receiverPtr);
    kSynchMesgUpdateReceiverPrio_(receiverPtr);
}

VOID kSynchMesgTimeoutCall(RK_TCB *const callerPtr)
{
    RK_TCB *const serverPtr = callerPtr->synchMesgReceiverPtr;
    if (serverPtr == NULL)
    {
        return;
    }

    if (callerPtr->synchMesgCallState == RK_SYNCH_CALL_QUEUED)
    {
        RK_TCB *remPtr = callerPtr;
        RK_ERR err = kTCBQRem(&serverPtr->synchMesgCallers, &remPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        kSynchMesgClearCall_(callerPtr);
        kSynchMesgUpdateReceiverPrio_(serverPtr);
        return;
    }

    if ((callerPtr->synchMesgCallState == RK_SYNCH_CALL_ACTIVE) &&
        (serverPtr->synchMesgActiveCallerPtr == callerPtr))
    {
        callerPtr->synchMesgPtr = NULL;
        callerPtr->synchMesgBytes = 0UL;
        callerPtr->synchMesgCallReplyBufPtr = NULL;
        callerPtr->synchMesgCallReplyBytesPtr = NULL;
        callerPtr->synchMesgCallReplyMaxBytes = 0UL;
        callerPtr->synchMesgStatus = RK_ERR_TIMEOUT;
        callerPtr->synchMesgCallState = RK_SYNCH_CALL_ABANDONED;
        kSynchMesgUpdateReceiverPrio_(serverPtr);
    }
}

static VOID kSynchMesgReadyWithStatus_(RK_TCB *const taskPtr,
                                       RK_ERR const status)
{
    if (taskPtr == NULL)
    {
        return;
    }

    if (kTimeoutNodeIsArmed(&taskPtr->timeoutNode) == RK_TRUE)
    {
        kTimeoutNodeDisarm(&taskPtr->timeoutNode);
    }
    else
    {
        kTimeoutNodeReset(&taskPtr->timeoutNode);
    }

    taskPtr->timeOut = RK_FALSE;
    taskPtr->synchMesgStatus = status;
    kReadySwtch(taskPtr);
}

VOID kSynchMesgTaskCleanup(RK_TCB *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return;
    }

    if ((taskPtr->synchMesgReceiverPtr != NULL) &&
        (taskPtr->synchMesgCallState == RK_SYNCH_CALL_ACTIVE) &&
        (taskPtr->synchMesgReceiverPtr->synchMesgActiveCallerPtr == taskPtr))
    {
        RK_TCB *const serverPtr = taskPtr->synchMesgReceiverPtr;
        kSynchMesgClearActiveCall_(serverPtr);
        kSynchMesgUpdateReceiverPrio_(serverPtr);
        kSynchMesgClearCall_(taskPtr);
    }
    else if (taskPtr->synchMesgReceiverPtr != NULL)
    {
        if (taskPtr->synchMesgCallState == RK_SYNCH_CALL_IDLE)
        {
            kSynchMesgTimeoutSend(taskPtr);
        }
        else
        {
            kSynchMesgTimeoutCall(taskPtr);
        }
    }

    while (taskPtr->synchMesgSenders.size > 0UL)
    {
        RK_TCB *senderPtr = NULL;
        RK_ERR err = kTCBQDeq(&taskPtr->synchMesgSenders, &senderPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if ((err != RK_ERR_SUCCESS) || (senderPtr == NULL))
        {
            break;
        }

        if (taskPtr->synchMesgPendingSenderPtr == senderPtr)
        {
            taskPtr->synchMesgPendingPtr = NULL;
            taskPtr->synchMesgPendingSenderPtr = NULL;
        }

        kSynchMesgClearSender_(senderPtr);
        kSynchMesgReadyWithStatus_(senderPtr, RK_ERR_OBJ_NOT_INIT);
    }

    while (taskPtr->synchMesgCallers.size > 0UL)
    {
        RK_TCB *callerPtr = NULL;
        RK_ERR err = kTCBQDeq(&taskPtr->synchMesgCallers, &callerPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if ((err != RK_ERR_SUCCESS) || (callerPtr == NULL))
        {
            break;
        }

        kSynchMesgClearCall_(callerPtr);
        kSynchMesgReadyWithStatus_(callerPtr, RK_ERR_OBJ_NOT_INIT);
    }

    if (taskPtr->synchMesgActiveCallerPtr != NULL)
    {
        RK_TCB *const callerPtr = taskPtr->synchMesgActiveCallerPtr;
        kSynchMesgClearActiveCall_(taskPtr);
        kSynchMesgClearCall_(callerPtr);
        kSynchMesgReadyWithStatus_(callerPtr, RK_ERR_OBJ_NOT_INIT);
    }

    while (taskPtr->synchMesgAcceptWaiters.size > 0UL)
    {
        RK_TCB *waiterPtr = NULL;
        RK_ERR err = kTCBQDeq(&taskPtr->synchMesgAcceptWaiters, &waiterPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if ((err != RK_ERR_SUCCESS) || (waiterPtr == NULL) ||
            (waiterPtr == taskPtr))
        {
            break;
        }

        kSynchMesgReadyWithStatus_(waiterPtr, RK_ERR_OBJ_NOT_INIT);
    }

    kSynchMesgClearReceiverWait_(taskPtr);
    kSynchMesgClearSender_(taskPtr);
    kSynchMesgClearCall_(taskPtr);
    kSynchMesgClearActiveCall_(taskPtr);
    taskPtr->synchMesgMaxBytes = 0UL;
    taskPtr->synchMesgPendingPtr = NULL;
    taskPtr->synchMesgPendingSenderPtr = NULL;
    taskPtr->synchMesgRecvStatus = RK_ERR_SUCCESS;
    kListInit(&taskPtr->synchMesgSenders);
    kListInit(&taskPtr->synchMesgCallers);
    kListInit(&taskPtr->synchMesgAcceptWaiters);
}

RK_ERR kSynchMesgInit(RK_TASK_HANDLE const taskHandle,
                      ULONG const maxMesgBytes)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SYNCH_MESG_INIT,
                                        (ULONG)(UINTPTR)taskHandle,
                                        maxMesgBytes, 0UL, 0UL));
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

    if (kSynchMesgBytesValid_(maxMesgBytes) == RK_FALSE)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }
#endif

    if (taskHandle == NULL)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_OBJ_NULL));
    }

    if (kIsISR())
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (kSynchMesgBytesValid_(maxMesgBytes) == RK_FALSE)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_INVALID_PARAM));
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR err = kTaskHandleResolve(taskHandle, &taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    if (taskPtr->synchMesgMaxBytes != 0UL)
    {
        RK_CR_EXIT
        return (RK_ERR_HAS_OWNER);
    }

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
    if (taskPtr->asynchMesgInit == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_HAS_OWNER);
    }
#if (RK_CONF_ASYNCH_COPY_MESG == ON)
    if (taskPtr->asynchCopyMesgInit == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_HAS_OWNER);
    }
#endif
#endif

    taskPtr->synchMesgMaxBytes = maxMesgBytes;
    taskPtr->synchMesgPendingPtr = NULL;
    taskPtr->synchMesgPendingSenderPtr = NULL;
    taskPtr->synchMesgRecvBufPtr = NULL;
    taskPtr->synchMesgRecvBytesPtr = NULL;
    taskPtr->synchMesgRecvStatus = RK_ERR_SUCCESS;
    err = kTCBQInit(&taskPtr->synchMesgSenders);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    err = kTCBQInit(&taskPtr->synchMesgCallers);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    err = kTCBQInit(&taskPtr->synchMesgAcceptWaiters);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    kSynchMesgClearActiveCall_(taskPtr);

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSynchSendWait(RK_TASK_HANDLE const taskHandle,
                      VOID const *const mesgPtr,
                      ULONG const mesgBytes,
                      RK_TICK const timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SYNCH_SEND_WAIT,
                                        (ULONG)(UINTPTR)taskHandle,
                                        (ULONG)(UINTPTR)mesgPtr,
                                        mesgBytes, (ULONG)timeout));
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

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
        RK_CR_EXIT
        return (RK_ERR_INVALID_TIMEOUT);
    }

    if (K_BLOCKING_ON_ISR(timeout))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }
#endif

    if ((taskHandle == NULL) || (mesgPtr == NULL))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_OBJ_NULL));
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_INVALID_TIMEOUT));
    }

    RK_ERR const currentErr = kSynchMesgCurrentTaskErr_(timeout);
    if (currentErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(currentErr));
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR resolveErr = kTaskHandleResolve(taskHandle, &taskPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (resolveErr);
    }

    if (taskPtr == RK_gRunPtr)
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    if (taskPtr->synchMesgMaxBytes == 0UL)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if ((kSynchMesgBytesValid_(mesgBytes) == RK_FALSE) ||
        (mesgBytes > taskPtr->synchMesgMaxBytes))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_MSG_SIZE);
    }

    if (kSynchMesgTaskOwnsMutex_(RK_gRunPtr) == RK_TRUE)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_TASK_INVALID_ST));
    }

    if ((RK_gRunPtr->synchMesgReceiverPtr != NULL) ||
        (RK_gRunPtr->synchMesgCallState != RK_SYNCH_CALL_IDLE))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_TASK_INVALID_ST));
    }

    if ((taskPtr->synchMesgRecvBufPtr != NULL) &&
        (taskPtr->synchMesgPendingPtr == NULL))
    {
        RK_ERR const err =
            kSynchMesgDirectRecv_(taskPtr, mesgPtr, mesgBytes);
        RK_CR_EXIT
        return (err);
    }

    if (timeout == RK_NO_WAIT)
    {
        RK_CR_EXIT
        return (RK_ERR_NOWAIT);
    }

    RK_gRunPtr->synchMesgPtr = mesgPtr;
    RK_gRunPtr->synchMesgBytes = mesgBytes;
    RK_gRunPtr->synchMesgStatus = RK_ERR_SUCCESS;
    RK_gRunPtr->synchMesgReceiverPtr = taskPtr;

    if (timeout != RK_WAIT_FOREVER)
    {
        RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_SYNCH_SEND;
        RK_gRunPtr->timeoutNode.waitingQueuePtr =
            &taskPtr->synchMesgSenders;
        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            kSynchMesgClearSender_(RK_gRunPtr);
            RK_CR_EXIT
            return (err);
        }
    }

    RK_gRunPtr->status = RK_SENDING;
    RK_ERR enqErr = kTCBQEnqByPrio(&taskPtr->synchMesgSenders,
                                   RK_gRunPtr);
    if (enqErr != RK_ERR_SUCCESS)
    {
        if (timeout != RK_WAIT_FOREVER)
        {
            kSynchMesgDisarmTimeout_(RK_gRunPtr);
        }
        kSynchMesgClearSender_(RK_gRunPtr);
        RK_CR_EXIT
        return (enqErr);
    }

    kSynchMesgPromoteNext_(taskPtr);
    kSynchMesgUpdateReceiverPrio_(taskPtr);
    kPendCtxSwtch();

    RK_CR_EXIT
    RK_CR_ENTER
    if (RK_gRunPtr->timeOut)
    {
        RK_gRunPtr->timeOut = RK_FALSE;
        RK_CR_EXIT
        return (RK_ERR_TIMEOUT);
    }

    RK_ERR const err = RK_gRunPtr->synchMesgStatus;
    RK_gRunPtr->synchMesgStatus = RK_ERR_SUCCESS;
    RK_CR_EXIT
    return (err);
}

RK_ERR kSynchSendWaitSyscall(RK_EXCEPTION_FRAME *const framePtr,
                             RK_TASK_HANDLE const taskHandle,
                             VOID const *const mesgPtr,
                             ULONG const mesgBytes,
                             RK_TICK const timeout)
{
    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if ((taskHandle == NULL) || (mesgPtr == NULL))
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }
#endif

    if ((taskHandle == NULL) || (mesgPtr == NULL))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR resolveErr = kTaskHandleResolve(taskHandle, &taskPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (resolveErr);
    }

    if (taskPtr->synchMesgMaxBytes == 0UL)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if ((taskPtr == RK_gRunPtr) ||
        (kSynchMesgTaskOwnsMutex_(RK_gRunPtr) == RK_TRUE) ||
        (RK_gRunPtr->synchMesgReceiverPtr != NULL) ||
        (RK_gRunPtr->synchMesgCallState != RK_SYNCH_CALL_IDLE))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return ((taskPtr == RK_gRunPtr) ? RK_ERR_INVALID_PARAM :
                                          RK_ERR_TASK_INVALID_ST);
    }

    if ((kSynchMesgBytesValid_(mesgBytes) == RK_FALSE) ||
        (mesgBytes > taskPtr->synchMesgMaxBytes))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_MSG_SIZE);
    }

    RK_ERR const userReadErr = kSynchMesgUserReadValid_(mesgPtr, mesgBytes);
    if (userReadErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userReadErr);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }

    if ((taskPtr->synchMesgRecvBufPtr != NULL) &&
        (taskPtr->synchMesgPendingPtr == NULL))
    {
        RK_ERR const err =
            kSynchMesgDirectRecv_(taskPtr, mesgPtr, mesgBytes);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (err);
    }

    if (timeout == RK_NO_WAIT)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_NOWAIT);
    }

    RK_gRunPtr->synchMesgPtr = mesgPtr;
    RK_gRunPtr->synchMesgBytes = mesgBytes;
    RK_gRunPtr->synchMesgStatus = RK_ERR_SUCCESS;
    RK_gRunPtr->synchMesgReceiverPtr = taskPtr;

    if (timeout != RK_WAIT_FOREVER)
    {
        RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_SYNCH_SEND;
        RK_gRunPtr->timeoutNode.waitingQueuePtr =
            &taskPtr->synchMesgSenders;
        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            kSynchMesgClearSender_(RK_gRunPtr);
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (err);
        }
    }

    RK_gRunPtr->status = RK_SENDING;
    RK_ERR enqErr = kTCBQEnqByPrio(&taskPtr->synchMesgSenders,
                                   RK_gRunPtr);
    if (enqErr != RK_ERR_SUCCESS)
    {
        if (timeout != RK_WAIT_FOREVER)
        {
            kSynchMesgDisarmTimeout_(RK_gRunPtr);
        }
        kSynchMesgClearSender_(RK_gRunPtr);
        RK_gRunPtr->status = RK_RUNNING;
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (enqErr);
    }

    kSynchMesgPromoteNext_(taskPtr);
    kSynchMesgUpdateReceiverPrio_(taskPtr);
    kSyscallTaskSuspend(framePtr, RK_SYSCALL_SYNCH_SEND_WAIT,
                        (ULONG)(UINTPTR)taskHandle,
                        (ULONG)(UINTPTR)mesgPtr,
                        mesgBytes, (ULONG)timeout,
                        RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();

    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

RK_ERR kSyncRecv(VOID *const recvPtr,
                 ULONG *const mesgBytesPtr,
                 RK_TICK const timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SYNCH_RECV,
                                        (ULONG)(UINTPTR)recvPtr,
                                        (ULONG)(UINTPTR)mesgBytesPtr,
                                        (ULONG)timeout, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if (recvPtr == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (RK_gRunPtr->synchMesgMaxBytes == 0UL)
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

    if (K_BLOCKING_ON_ISR(timeout))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }
#endif

    if (recvPtr == NULL)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_OBJ_NULL));
    }

    RK_ERR const currentErr = kSynchMesgCurrentTaskErr_(timeout);
    if (currentErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(currentErr));
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_INVALID_TIMEOUT));
    }

    if (RK_gRunPtr->synchMesgMaxBytes == 0UL)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_OBJ_NOT_INIT));
    }

    if (kSynchMesgTaskOwnsMutex_(RK_gRunPtr) == RK_TRUE)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_TASK_INVALID_ST));
    }

    if (RK_gRunPtr->synchMesgPendingPtr != NULL)
    {
        RK_ERR err = kSynchMesgConsumePendingSend_(RK_gRunPtr, recvPtr,
                                                    mesgBytesPtr);
        RK_CR_EXIT
        return (err);
    }

    if (timeout == RK_NO_WAIT)
    {
        RK_CR_EXIT
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_gRunPtr->synchMesgRecvBufPtr = recvPtr;
    RK_gRunPtr->synchMesgRecvBytesPtr = mesgBytesPtr;
    RK_gRunPtr->synchMesgRecvStatus = RK_ERR_SUCCESS;
    if (timeout != RK_WAIT_FOREVER)
    {
        RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_SYNCH_RECV;
        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            kSynchMesgClearReceiverWait_(RK_gRunPtr);
            RK_CR_EXIT
            return (err);
        }
    }

    RK_gRunPtr->status = RK_RECEIVING;
    kPendCtxSwtch();

    RK_CR_EXIT
    RK_CR_ENTER
    if (RK_gRunPtr->timeOut)
    {
        RK_gRunPtr->timeOut = RK_FALSE;
        RK_CR_EXIT
        return (RK_ERR_TIMEOUT);
    }

    if (RK_gRunPtr->synchMesgRecvBufPtr == NULL)
    {
        RK_ERR const err = RK_gRunPtr->synchMesgRecvStatus;
        RK_gRunPtr->synchMesgRecvStatus = RK_ERR_SUCCESS;
        RK_CR_EXIT
        return (err);
    }

    RK_ERR err = kSynchMesgConsumePendingSend_(RK_gRunPtr, recvPtr,
                                                mesgBytesPtr);
    kSynchMesgClearReceiverWait_(RK_gRunPtr);
    RK_CR_EXIT
    return (err);
}

RK_ERR kSyncRecvSyscall(RK_EXCEPTION_FRAME *const framePtr,
                        VOID *const recvPtr,
                        ULONG *const mesgBytesPtr,
                        RK_TICK const timeout)
{
    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if (recvPtr == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    if ((RK_gRunPtr == NULL) ||
        (RK_gRunPtr->synchMesgMaxBytes == 0UL))
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

    if (recvPtr == NULL)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    if ((RK_gRunPtr == NULL) ||
        (RK_gRunPtr->synchMesgMaxBytes == 0UL))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NOT_INIT);
    }

    RK_ERR const userRecvErr =
        kSynchMesgUserWriteValid_(recvPtr, RK_gRunPtr->synchMesgMaxBytes);
    if (userRecvErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userRecvErr);
    }

    RK_ERR const userBytesErr =
        kSynchMesgUserWriteOptional_(mesgBytesPtr, sizeof(ULONG));
    if (userBytesErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userBytesErr);
    }

    if (kSynchMesgTaskOwnsMutex_(RK_gRunPtr) == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_TASK_INVALID_STATE);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_TASK_INVALID_ST);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }

    if (RK_gRunPtr->synchMesgPendingPtr != NULL)
    {
        RK_ERR err = kSynchMesgConsumePendingSend_(RK_gRunPtr, recvPtr,
                                                   mesgBytesPtr);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (err);
    }

    if (timeout == RK_NO_WAIT)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_gRunPtr->synchMesgRecvBufPtr = recvPtr;
    RK_gRunPtr->synchMesgRecvBytesPtr = mesgBytesPtr;
    RK_gRunPtr->synchMesgRecvStatus = RK_ERR_SUCCESS;
    if (timeout != RK_WAIT_FOREVER)
    {
        RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_SYNCH_RECV;
        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            kSynchMesgClearReceiverWait_(RK_gRunPtr);
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (err);
        }
    }

    RK_gRunPtr->status = RK_RECEIVING;
    kSyscallTaskSuspend(framePtr, RK_SYSCALL_SYNCH_RECV,
                        (ULONG)(UINTPTR)recvPtr,
                        (ULONG)(UINTPTR)mesgBytesPtr,
                        (ULONG)timeout, 0UL, RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();

    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

RK_ERR kSynchMesgCall(RK_TASK_HANDLE const taskHandle,
                      RK_SYNCH_ATTR const *const attrPtr,
                      RK_TICK const timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SYNCH_MESG_CALL,
                                        (ULONG)(UINTPTR)taskHandle,
                                        (ULONG)(UINTPTR)attrPtr,
                                        (ULONG)timeout, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

    if ((taskHandle == NULL) || (attrPtr == NULL) ||
        (attrPtr->reqPtr == NULL) || (attrPtr->replyPtr == NULL))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_OBJ_NULL));
    }

    RK_ERR const currentErr = kSynchMesgCurrentTaskErr_(timeout);
    if (currentErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(currentErr));
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR resolveErr = kTaskHandleResolve(taskHandle, &taskPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        RK_CR_EXIT
        return (resolveErr);
    }

    if (taskPtr == RK_gRunPtr)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_INVALID_PARAM));
    }

    if (taskPtr->synchMesgMaxBytes == 0UL)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_OBJ_NOT_INIT));
    }

    if ((kSynchMesgBytesValid_(attrPtr->reqBytes) == RK_FALSE) ||
        (attrPtr->reqBytes > taskPtr->synchMesgMaxBytes) ||
        (kSynchMesgBytesValid_(attrPtr->replyMaxBytes) == RK_FALSE))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_INVALID_MSG_SIZE));
    }

    if ((timeout == RK_NO_WAIT) ||
        ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD)))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_INVALID_TIMEOUT));
    }

    if ((kSynchMesgTaskOwnsMutex_(RK_gRunPtr) == RK_TRUE) ||
        (RK_gRunPtr->synchMesgReceiverPtr != NULL) ||
        (RK_gRunPtr->synchMesgCallState != RK_SYNCH_CALL_IDLE))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_TASK_INVALID_ST));
    }

    if (attrPtr->replyBytesPtr != NULL)
    {
        *attrPtr->replyBytesPtr = 0UL;
    }

    RK_gRunPtr->synchMesgPtr = attrPtr->reqPtr;
    RK_gRunPtr->synchMesgBytes = attrPtr->reqBytes;
    RK_gRunPtr->synchMesgStatus = RK_ERR_SUCCESS;
    RK_gRunPtr->synchMesgReceiverPtr = taskPtr;
    RK_gRunPtr->synchMesgCallReplyBufPtr = attrPtr->replyPtr;
    RK_gRunPtr->synchMesgCallReplyBytesPtr = attrPtr->replyBytesPtr;
    RK_gRunPtr->synchMesgCallReplyMaxBytes = attrPtr->replyMaxBytes;
    RK_gRunPtr->synchMesgCallState = RK_SYNCH_CALL_QUEUED;

    if (timeout != RK_WAIT_FOREVER)
    {
        RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_SYNCH_CALL;
        RK_gRunPtr->timeoutNode.waitingQueuePtr =
            &taskPtr->synchMesgCallers;
        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            kSynchMesgClearCall_(RK_gRunPtr);
            RK_CR_EXIT
            return (err);
        }
    }

    RK_gRunPtr->status = RK_RECEIVING;
    RK_ERR enqErr = kTCBQEnqByPrio(&taskPtr->synchMesgCallers,
                                   RK_gRunPtr);
    if (enqErr != RK_ERR_SUCCESS)
    {
        if (timeout != RK_WAIT_FOREVER)
        {
            kSynchMesgDisarmTimeout_(RK_gRunPtr);
        }
        kSynchMesgClearCall_(RK_gRunPtr);
        RK_CR_EXIT
        return (enqErr);
    }

    kSynchMesgUpdateReceiverPrio_(taskPtr);
    kSynchMesgWakeAcceptor_(taskPtr);
    kPendCtxSwtch();

    RK_CR_EXIT
    RK_CR_ENTER
    if (RK_gRunPtr->timeOut)
    {
        RK_gRunPtr->timeOut = RK_FALSE;
        RK_CR_EXIT
        return (RK_ERR_TIMEOUT);
    }

    RK_ERR const err = RK_gRunPtr->synchMesgStatus;
    RK_gRunPtr->synchMesgStatus = RK_ERR_SUCCESS;
    RK_CR_EXIT
    return (err);
}

RK_ERR kSynchMesgCallSyscall(RK_EXCEPTION_FRAME *const framePtr,
                             RK_TASK_HANDLE const taskHandle,
                             RK_SYNCH_ATTR const *const attrPtr,
                             RK_TICK const timeout)
{
    RK_SYNCH_ATTR attr;

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR userErr = kSynchMesgUserReadValid_(attrPtr, sizeof(attr));
    if (userErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userErr);
    }
    RK_MEMCPY(&attr, attrPtr, sizeof(attr));

    if ((taskHandle == NULL) || (attr.reqPtr == NULL) ||
        (attr.replyPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR resolveErr = kTaskHandleResolve(taskHandle, &taskPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (resolveErr);
    }

    if (taskPtr == RK_gRunPtr)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_PARAM);
    }

    if (taskPtr->synchMesgMaxBytes == 0UL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if ((kSynchMesgBytesValid_(attr.reqBytes) == RK_FALSE) ||
        (attr.reqBytes > taskPtr->synchMesgMaxBytes) ||
        (kSynchMesgBytesValid_(attr.replyMaxBytes) == RK_FALSE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_MSG_SIZE);
    }

    userErr = kSynchMesgUserReadValid_(attr.reqPtr, attr.reqBytes);
    if (userErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userErr);
    }

    userErr = kSynchMesgUserWriteValid_(attr.replyPtr,
                                        attr.replyMaxBytes);
    if (userErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userErr);
    }

    userErr = kSynchMesgUserWriteOptional_(attr.replyBytesPtr,
                                           sizeof(ULONG));
    if (userErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (userErr);
    }

    if ((timeout == RK_NO_WAIT) ||
        ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD)))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }

    if ((kSynchMesgTaskOwnsMutex_(RK_gRunPtr) == RK_TRUE) ||
        (RK_gRunPtr->synchMesgReceiverPtr != NULL) ||
        (RK_gRunPtr->synchMesgCallState != RK_SYNCH_CALL_IDLE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_TASK_INVALID_STATE);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_TASK_INVALID_ST);
    }

    if (attr.replyBytesPtr != NULL)
    {
        *attr.replyBytesPtr = 0UL;
    }

    RK_gRunPtr->synchMesgPtr = attr.reqPtr;
    RK_gRunPtr->synchMesgBytes = attr.reqBytes;
    RK_gRunPtr->synchMesgStatus = RK_ERR_SUCCESS;
    RK_gRunPtr->synchMesgReceiverPtr = taskPtr;
    RK_gRunPtr->synchMesgCallReplyBufPtr = attr.replyPtr;
    RK_gRunPtr->synchMesgCallReplyBytesPtr = attr.replyBytesPtr;
    RK_gRunPtr->synchMesgCallReplyMaxBytes = attr.replyMaxBytes;
    RK_gRunPtr->synchMesgCallState = RK_SYNCH_CALL_QUEUED;

    if (timeout != RK_WAIT_FOREVER)
    {
        RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_SYNCH_CALL;
        RK_gRunPtr->timeoutNode.waitingQueuePtr =
            &taskPtr->synchMesgCallers;
        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            kSynchMesgClearCall_(RK_gRunPtr);
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (err);
        }
    }

    RK_gRunPtr->status = RK_RECEIVING;
    RK_ERR enqErr = kTCBQEnqByPrio(&taskPtr->synchMesgCallers,
                                   RK_gRunPtr);
    if (enqErr != RK_ERR_SUCCESS)
    {
        if (timeout != RK_WAIT_FOREVER)
        {
            kSynchMesgDisarmTimeout_(RK_gRunPtr);
        }
        kSynchMesgClearCall_(RK_gRunPtr);
        RK_gRunPtr->status = RK_RUNNING;
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (enqErr);
    }

    kSynchMesgUpdateReceiverPrio_(taskPtr);
    kSynchMesgWakeAcceptor_(taskPtr);
    kSyscallTaskSuspend(framePtr, RK_SYSCALL_SYNCH_MESG_CALL,
                        (ULONG)(UINTPTR)taskHandle,
                        (ULONG)(UINTPTR)attrPtr, (ULONG)timeout, 0UL,
                        RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();

    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

RK_ERR kSynchMesgAccept(RK_SYNCH_CALL_DATA *const callPtr,
                        VOID *const recvPtr,
                        ULONG *const reqBytesPtr,
                        RK_TICK const timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SYNCH_MESG_ACCEPT,
                                        (ULONG)(UINTPTR)callPtr,
                                        (ULONG)(UINTPTR)recvPtr,
                                        (ULONG)(UINTPTR)reqBytesPtr,
                                        (ULONG)timeout));
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if ((callPtr == NULL) || (recvPtr == NULL))
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (RK_gRunPtr->synchMesgMaxBytes == 0UL)
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

    if (K_BLOCKING_ON_ISR(timeout))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }
#endif

    if ((callPtr == NULL) || (recvPtr == NULL))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_OBJ_NULL));
    }

    RK_ERR const currentErr = kSynchMesgCurrentTaskErr_(timeout);
    if (currentErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(currentErr));
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_INVALID_TIMEOUT));
    }

    if (RK_gRunPtr->synchMesgMaxBytes == 0UL)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_OBJ_NOT_INIT));
    }

    if (kSynchMesgTaskOwnsMutex_(RK_gRunPtr) == RK_TRUE)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_TASK_INVALID_ST));
    }

    if (RK_gRunPtr->synchMesgActiveCallerPtr != NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_SYNCH_CALL_BUSY);
    }

    while (RK_gRunPtr->synchMesgCallers.size == 0UL)
    {
        if (timeout == RK_NO_WAIT)
        {
            RK_CR_EXIT
            return (RK_ERR_BUFFER_EMPTY);
        }

        if (timeout != RK_WAIT_FOREVER)
        {
            RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
            RK_gRunPtr->timeoutNode.waitingQueuePtr =
                &RK_gRunPtr->synchMesgAcceptWaiters;
            RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
                RK_CR_EXIT
                return (err);
            }
        }

        RK_gRunPtr->status = RK_RECEIVING;
        RK_ERR err = kTCBQEnq(&RK_gRunPtr->synchMesgAcceptWaiters,
                              RK_gRunPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if (err != RK_ERR_SUCCESS)
        {
            if (timeout != RK_WAIT_FOREVER)
            {
                kSynchMesgDisarmTimeout_(RK_gRunPtr);
            }
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
            RK_CR_EXIT
            return (RK_ERR_TIMEOUT);
        }
    }

    RK_TCB *callerPtr = kTCBQPeek(&RK_gRunPtr->synchMesgCallers);
    K_ASSERT(callerPtr != NULL);
    ULONG const reqBytes = callerPtr->synchMesgBytes;
    kSynchMesgCopy_(recvPtr, callerPtr->synchMesgPtr, reqBytes);
    kSynchMesgRecvBytesSet_(reqBytesPtr, reqBytes);

    RK_TCB *remPtr = callerPtr;
    RK_ERR err = kTCBQRem(&RK_gRunPtr->synchMesgCallers, &remPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    callerPtr->synchMesgPtr = NULL;
    callerPtr->synchMesgBytes = 0UL;
    callerPtr->synchMesgCallState = RK_SYNCH_CALL_ACTIVE;
    /*
     * Accept starts the extended call/reply rendezvous. From here until reply,
     * timeout, or cleanup, the server runs at the captured caller effective
     * priority.
     */
    RK_gRunPtr->synchMesgActiveCallerPtr = callerPtr;
    RK_gRunPtr->synchMesgActiveCallerPrio = callerPtr->priority;

    callPtr->caller = kTaskHandleFromTcb(callerPtr);
    callPtr->reqPtr = recvPtr;
    callPtr->replyPtr = callerPtr->synchMesgCallReplyBufPtr;
    callPtr->reqBytes = reqBytes;
    callPtr->replyMaxBytes = callerPtr->synchMesgCallReplyMaxBytes;

    kSynchMesgUpdateReceiverPrio_(RK_gRunPtr);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSynchMesgAcceptSyscall(RK_EXCEPTION_FRAME *const framePtr,
                               RK_SYNCH_CALL_DATA *const callPtr,
                               VOID *const recvPtr,
                               ULONG *const reqBytesPtr,
                               RK_TICK const timeout)
{
    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if ((callPtr == NULL) || (recvPtr == NULL))
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    if ((RK_gRunPtr == NULL) ||
        (RK_gRunPtr->synchMesgMaxBytes == 0UL))
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

    if ((callPtr == NULL) || (recvPtr == NULL))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NULL);
    }

    if ((RK_gRunPtr == NULL) ||
        (RK_gRunPtr->synchMesgMaxBytes == 0UL))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NOT_INIT);
    }

    RK_ERR const callWriteErr =
        kSynchMesgUserWriteValid_(callPtr, sizeof(RK_SYNCH_CALL_DATA));
    if (callWriteErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (callWriteErr);
    }

    RK_ERR const recvWriteErr =
        kSynchMesgUserWriteValid_(recvPtr, RK_gRunPtr->synchMesgMaxBytes);
    if (recvWriteErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (recvWriteErr);
    }

    RK_ERR const bytesWriteErr =
        kSynchMesgUserWriteOptional_(reqBytesPtr, sizeof(ULONG));
    if (bytesWriteErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (bytesWriteErr);
    }

    if (kSynchMesgTaskOwnsMutex_(RK_gRunPtr) == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_TASK_INVALID_STATE);
#endif
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_TASK_INVALID_ST);
    }

    if (RK_gRunPtr->synchMesgActiveCallerPtr != NULL)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_SYNCH_CALL_BUSY);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }

    if (RK_gRunPtr->synchMesgCallers.size == 0UL)
    {
        if (timeout == RK_NO_WAIT)
        {
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (RK_ERR_BUFFER_EMPTY);
        }

        if (timeout != RK_WAIT_FOREVER)
        {
            RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
            RK_gRunPtr->timeoutNode.waitingQueuePtr =
                &RK_gRunPtr->synchMesgAcceptWaiters;
            RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
                RK_CR_EXIT
                kSyscallTaskClear(RK_gRunPtr);
                return (err);
            }
        }

        RK_gRunPtr->status = RK_RECEIVING;
        RK_ERR err = kTCBQEnq(&RK_gRunPtr->synchMesgAcceptWaiters,
                              RK_gRunPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if (err != RK_ERR_SUCCESS)
        {
            if (timeout != RK_WAIT_FOREVER)
            {
                kSynchMesgDisarmTimeout_(RK_gRunPtr);
            }
            RK_gRunPtr->status = RK_RUNNING;
            RK_CR_EXIT
            kSyscallTaskClear(RK_gRunPtr);
            return (err);
        }

        kSyscallTaskSuspend(framePtr, RK_SYSCALL_SYNCH_MESG_ACCEPT,
                            (ULONG)(UINTPTR)callPtr,
                            (ULONG)(UINTPTR)recvPtr,
                            (ULONG)(UINTPTR)reqBytesPtr,
                            (ULONG)timeout, RK_SYSCALL_PHASE_WAIT);
        kPendCtxSwtch();
        RK_CR_EXIT
        return (RK_ERR_SYSCALL_RESTART);
    }

    RK_TCB *callerPtr = kTCBQPeek(&RK_gRunPtr->synchMesgCallers);
    K_ASSERT(callerPtr != NULL);
    ULONG const reqBytes = callerPtr->synchMesgBytes;
    kSynchMesgCopy_(recvPtr, callerPtr->synchMesgPtr, reqBytes);
    kSynchMesgRecvBytesSet_(reqBytesPtr, reqBytes);

    RK_TCB *remPtr = callerPtr;
    RK_ERR err = kTCBQRem(&RK_gRunPtr->synchMesgCallers, &remPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        kSyscallTaskClear(RK_gRunPtr);
        return (err);
    }

    callerPtr->synchMesgPtr = NULL;
    callerPtr->synchMesgBytes = 0UL;
    callerPtr->synchMesgCallState = RK_SYNCH_CALL_ACTIVE;
    /*
     * Accept starts the extended call/reply rendezvous. From here until reply,
     * timeout, or cleanup, the server runs at the captured caller effective
     * priority.
     */
    RK_gRunPtr->synchMesgActiveCallerPtr = callerPtr;
    RK_gRunPtr->synchMesgActiveCallerPrio = callerPtr->priority;

    callPtr->caller = kTaskHandleFromTcb(callerPtr);
    callPtr->reqPtr = recvPtr;
    callPtr->replyPtr = callerPtr->synchMesgCallReplyBufPtr;
    callPtr->reqBytes = reqBytes;
    callPtr->replyMaxBytes = callerPtr->synchMesgCallReplyMaxBytes;

    kSynchMesgUpdateReceiverPrio_(RK_gRunPtr);
    RK_CR_EXIT
    kSyscallTaskClear(RK_gRunPtr);
    return (RK_ERR_SUCCESS);
}

RK_ERR kSynchMesgReply(RK_SYNCH_CALL_DATA const *const callPtr,
                       VOID const *const replyPtr,
                       ULONG const replyBytes)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SYNCH_MESG_REPLY,
                                        (ULONG)(UINTPTR)callPtr,
                                        (ULONG)(UINTPTR)replyPtr,
                                        replyBytes, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)
    if (callPtr == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }
#endif

    if (callPtr == NULL)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_OBJ_NULL));
    }

    if ((kIsISR()) || (RK_gRunPtr == NULL))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    RK_TCB *callerPtr = NULL;
    RK_ERR resolveErr = kTaskHandleResolve(callPtr->caller, &callerPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        RK_CR_EXIT
        return (resolveErr);
    }

    if ((callerPtr == NULL) ||
        (RK_gRunPtr->synchMesgActiveCallerPtr != callerPtr) ||
        (callerPtr->synchMesgReceiverPtr != RK_gRunPtr) ||
        ((callerPtr->synchMesgCallState != RK_SYNCH_CALL_ACTIVE) &&
         (callerPtr->synchMesgCallState != RK_SYNCH_CALL_ABANDONED)))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_SYNCH_CALL_NOT_ACTIVE));
    }

    if (kSynchMesgTaskOwnsMutex_(RK_gRunPtr) == RK_TRUE)
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_TASK_INVALID_ST));
    }

    if (callerPtr->synchMesgCallState == RK_SYNCH_CALL_ABANDONED)
    {
        kSynchMesgClearActiveCall_(RK_gRunPtr);
        kSynchMesgClearCall_(callerPtr);
        kSynchMesgUpdateReceiverPrio_(RK_gRunPtr);
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    if (((replyBytes > 0UL) && (replyPtr == NULL)) ||
        (replyBytes > callerPtr->synchMesgCallReplyMaxBytes) ||
        ((replyBytes > 0UL) && ((replyBytes % RK_WORD_SIZE) != 0UL)))
    {
        RK_CR_EXIT
        return (kSynchMesgReportErr_(RK_ERR_INVALID_MSG_SIZE));
    }

    if (replyBytes > 0UL)
    {
        kSynchMesgCopy_(callerPtr->synchMesgCallReplyBufPtr, replyPtr,
                        replyBytes);
    }
    kSynchMesgRecvBytesSet_(callerPtr->synchMesgCallReplyBytesPtr,
                            replyBytes);
    callerPtr->synchMesgStatus = RK_ERR_SUCCESS;

    if (callerPtr->timeoutNode.timeoutType == RK_TIMEOUT_SYNCH_CALL)
    {
        kSynchMesgDisarmTimeout_(callerPtr);
    }

    kSynchMesgClearActiveCall_(RK_gRunPtr);
    kSynchMesgClearCall_(callerPtr);
    kSynchMesgUpdateReceiverPrio_(RK_gRunPtr);

    RK_ERR err = kSynchMesgPublicReadyErr_(kReadySwtch(callerPtr));
    RK_CR_EXIT
    return (err);
}

#endif /* RK_CONF_SYNCH_MESG */
