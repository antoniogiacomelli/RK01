/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * File intent:
 *   Mutex implementation with optional priority inheritance. Fault cleanup
 *   poisons mutexes owned by a dead task so waiters see
 *   RK_ERR_MUTEX_OWNER_FAULTED instead of silently entering inconsistent state.
 *
 * Contracts/invariants:
 *   - A locked mutex has exactly one owner and appears once in that owner's
 *     ownedMutexList.
 *   - A waiter in waitingQueue has waitingForMutexPtr pointing back here.
 *   - Priority inheritance is recomputed whenever ownership or waiters change.
 *   - A faulted owner poisons the mutex; waiters are released with
 *     RK_ERR_MUTEX_OWNER_FAULTED and ownership is not transferred.
 */

#define RK_SOURCE_CODE

#include <kdynobjs.h>
#include <ktimer.h>
#include <ksch.h>
#include <ksyscall.h>
#include <ktrace.h>
#include <kmutex.h>

#if (RK_CONF_MUTEX == ON)

/******************************************************************************/
/* MUTEX LIST                                                                 */
/******************************************************************************/
/*
 * Keep ownership-list edits behind a narrow helper so every lock/unlock path
 * preserves the "owner list contains locked mutexes only" invariant.
 */
static inline RK_ERR kMutexListAdd(struct RK_STRUCT_LIST *ownedMutexList,
                                   struct RK_STRUCT_LIST_NODE *mutexNode)
{
    RK_DSB
    return kListAddTail(ownedMutexList, mutexNode);
}

static inline RK_ERR kMutexListRem(struct RK_STRUCT_LIST *ownedMutexList,
                                   struct RK_STRUCT_LIST_NODE *mutexNode)
{
    RK_DSB
    return kListRemove(ownedMutexList, mutexNode);
}

/******************************************************************************/
/* PRIORITY INHERITANCE                                                       */
/******************************************************************************/
static VOID kMutexUpdateOwnerPrio_(RK_TCB *ownerTcb)
{
    kTaskUpdateEffectivePrioChain(ownerTcb);
}

/* Convert the public mutex handle into a checked kernel object pointer. */
static RK_ERR kMutexResolve_(RK_MUTEX_HANDLE const mutexHandle,
                             RK_MUTEX **const mutexPPtr)
{
    if (mutexPPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    *mutexPPtr = NULL;

    VOID *objPtr = NULL;
    RK_ERR const err =
        kDynObjResolveHandle(RK_DYN_OBJ_TYPE_MUTEX, mutexHandle, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
    *mutexPPtr = (RK_MUTEX *)objPtr;
    return (RK_ERR_SUCCESS);
}

static RK_ERR kMutexReadyErr_(RK_MUTEX const *const kobj)
{
    RK_ERR const err =
        kObjHeaderReadyErr((kobj != NULL) ? &kobj->header : NULL,
                           RK_MUTEX_KOBJ_ID);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    return (kObjHeaderDomainLocalAccessErr(
        &kobj->header, (RK_gRunPtr != NULL) ? RK_gRunPtr->domainPtr : NULL));
}

static RK_ERR kMutexReportErr_(RK_ERR const err)
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
    else if (err == RK_ERR_INVALID_PARAM)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
    }
    else if (err == RK_ERR_MUTEX_NOT_LOCKED)
    {
        K_ERR_HANDLER(RK_FAULT_MUTEX_NOT_LOCKED);
    }
    else if (err == RK_ERR_MUTEX_NOT_OWNER)
    {
        K_ERR_HANDLER(RK_FAULT_UNLOCK_OWNED_MUTEX);
    }
#else
    (VOID)err;
#endif
    return (err);
}

VOID kMutexTimeoutWaiter(RK_TCB *const waiterPtr)
{
    /*
     * Timeout removes a waiter from the dependency graph. If the mutex uses
     * priority inheritance, the owner's effective priority may drop.
     */
    if ((waiterPtr == NULL) || (waiterPtr->waitingForMutexPtr == NULL))
    {
        return;
    }

    RK_MUTEX *const mtxPtr = waiterPtr->waitingForMutexPtr;
    if ((mtxPtr->protocol == RK_PRIO_INHERITANCE) &&
        (mtxPtr->ownerPtr != NULL))
    {
        kMutexUpdateOwnerPrio_(mtxPtr->ownerPtr);
    }
}

/******************************************************************************/
/* MUTEX SEMAPHORE                                                            */
/******************************************************************************/
/* There is no recursive lock. Unlocking a mutex you do not own hard-faults. */
RK_ERR kMutexInit(RK_MUTEX *const kobj, UINT const protocol)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MUTEX_INIT,
                                        (ULONG)(UINTPTR)kobj,
                                        (ULONG)protocol, 0UL, 0UL));
    }

    RK_ERR const phaseErr = kKernelRawInitGuard();
    if (phaseErr != RK_ERR_SUCCESS)
    {
        return (phaseErr);
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

    if (kobj->init == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_DOUBLE_INIT);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_DOUBLE_INIT);
    }

    if ((protocol != RK_PRIO_NONE) && (protocol != RK_PRIO_INHERITANCE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    RK_ERR const queueErr = kTCBQInit(&(kobj->waitingQueue));
    if (queueErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (queueErr);
    }

    kobj->init = RK_TRUE;
    kobj->protocol = protocol;
    kobj->objID = RK_MUTEX_KOBJ_ID;
    kobj->objName[0] = '\0';
    kObjHeaderOwnerDomainSet(&kobj->header,
                             (RK_gRunPtr != NULL) ? RK_gRunPtr->domainPtr
                                                  : NULL);
    kobj->lock = RK_FALSE;
    kobj->ownerFaulted = RK_FALSE;
    kobj->ownerPtr = NULL;
    kTraceRegisterObject(kobj, RK_MUTEX_KOBJ_ID);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMutexLock(RK_MUTEX_HANDLE const mutexHandle, RK_TICK const timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MUTEX_LOCK,
                                        (ULONG)(UINTPTR)mutexHandle,
                                        (ULONG)timeout, 0UL, 0UL));
    }

    RK_MUTEX *kobj = NULL;
    RK_ERR const resolveErr = kMutexResolve_(mutexHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA

    RK_CR_ENTER

    RK_ERR const readyErr = kMutexReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        kMutexReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (kIsISR())
    {
        RK_CR_EXIT
        return (kMutexReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (kobj->ownerFaulted == RK_TRUE)
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_LOCK,
                           RK_ERR_MUTEX_OWNER_FAULTED,
                           kobj->waitingQueue.size);
        RK_CR_EXIT
        return (RK_ERR_MUTEX_OWNER_FAULTED);
    }

    if (kobj->lock == RK_FALSE)
    {
        kobj->lock = RK_TRUE;
        kobj->ownerPtr = RK_gRunPtr;
        kMutexListAdd(&RK_gRunPtr->ownedMutexList, &kobj->mutexNode);
        kTraceRecordObject(kobj, RK_TRACE_OP_LOCK, RK_ERR_SUCCESS,
                           kobj->waitingQueue.size);
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    if ((kobj->ownerPtr != RK_gRunPtr) && (kobj->ownerPtr != NULL))
    {
        if (timeout == 0UL)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_LOCK, RK_ERR_MUTEX_LOCKED,
                               kobj->waitingQueue.size);
            RK_CR_EXIT
            return (RK_ERR_MUTEX_LOCKED);
        }

        if ((timeout != RK_WAIT_FOREVER) && (timeout > 0UL))
        {
            RK_TASK_TIMEOUT_WAITINGQUEUE_SETUP

            RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                RK_gRunPtr->timeoutNode.timeoutType = 0;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                kTraceRecordObject(kobj, RK_TRACE_OP_LOCK, err,
                                   kobj->waitingQueue.size);
                RK_CR_EXIT
                return (err);
            }
        }
        if (timeout == RK_WAIT_FOREVER)
        {
            RK_gRunPtr->timeoutNode.waitingQueuePtr = &kobj->waitingQueue;
        }

        kTraceRecordObject(kobj, RK_TRACE_OP_LOCK_BLOCK, RK_ERR_SUCCESS,
                           kobj->waitingQueue.size + 1UL);
        kTCBQEnqByPrio(&kobj->waitingQueue, RK_gRunPtr);

        RK_gRunPtr->status = RK_BLOCKED;
        RK_gRunPtr->waitingForMutexPtr = kobj;
        if (kobj->protocol == RK_PRIO_INHERITANCE)
        {
            kMutexUpdateOwnerPrio_(kobj->ownerPtr);
        }

        kPendCtxSwtch();

        RK_CR_EXIT

        RK_CR_ENTER

        if (RK_gRunPtr->timeOut)
        {
            if ((kobj->protocol == RK_PRIO_INHERITANCE) &&
                (kobj->ownerPtr != NULL))
            {
                kMutexUpdateOwnerPrio_(kobj->ownerPtr);
            }

            RK_gRunPtr->timeOut = RK_FALSE;
            RK_gRunPtr->waitingForMutexPtr = NULL;

            kTraceRecordObject(kobj, RK_TRACE_OP_TIMEOUT, RK_ERR_TIMEOUT,
                               kobj->waitingQueue.size);
            RK_CR_EXIT
            return (RK_ERR_TIMEOUT);
        }

        if (kobj->ownerFaulted == RK_TRUE)
        {
            RK_gRunPtr->waitingForMutexPtr = NULL;
            kTraceRecordObject(kobj, RK_TRACE_OP_LOCK,
                               RK_ERR_MUTEX_OWNER_FAULTED,
                               kobj->waitingQueue.size);
            RK_CR_EXIT
            return (RK_ERR_MUTEX_OWNER_FAULTED);
        }

        if ((timeout != RK_WAIT_FOREVER) && (timeout > 0UL) &&
            (RK_gRunPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING))
        {
            kRemoveTimeoutNode(&RK_gRunPtr->timeoutNode);
            RK_gRunPtr->timeoutNode.timeoutType = 0;
            RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
        }
    }
    else if (kobj->ownerPtr == RK_gRunPtr)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_MUTEX_REC_LOCK);
#endif
        RK_CR_EXIT
        return (RK_ERR_MUTEX_REC_LOCK);
    }

    kTraceRecordObject(kobj, RK_TRACE_OP_LOCK, RK_ERR_SUCCESS,
                       kobj->waitingQueue.size);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMutexLockSyscallContinue(RK_EXCEPTION_FRAME *const framePtr,
                                 RK_MUTEX_HANDLE const mutexHandle,
                                 RK_TICK const timeout,
                                 ULONG const callNumber,
                                 ULONG const arg0,
                                 ULONG const arg1,
                                 ULONG const arg2,
                                 ULONG const arg3,
                                 UINT const phase)
{
    RK_MUTEX *kobj = NULL;
    RK_ERR const resolveErr = kMutexResolve_(mutexHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA

    RK_CR_ENTER

    RK_ERR const readyErr = kMutexReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        kMutexReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (kobj->ownerFaulted == RK_TRUE)
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_LOCK,
                           RK_ERR_MUTEX_OWNER_FAULTED,
                           kobj->waitingQueue.size);
        if (callNumber == RK_SYSCALL_MUTEX_LOCK)
        {
            kSyscallTaskClear(RK_gRunPtr);
        }
        RK_CR_EXIT
        return (RK_ERR_MUTEX_OWNER_FAULTED);
    }

    if (kobj->lock == RK_FALSE)
    {
        kobj->lock = RK_TRUE;
        kobj->ownerPtr = RK_gRunPtr;
        kMutexListAdd(&RK_gRunPtr->ownedMutexList, &kobj->mutexNode);
        kTraceRecordObject(kobj, RK_TRACE_OP_LOCK, RK_ERR_SUCCESS,
                           kobj->waitingQueue.size);
        if (callNumber == RK_SYSCALL_MUTEX_LOCK)
        {
            kSyscallTaskClear(RK_gRunPtr);
        }
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    if ((kobj->ownerPtr != RK_gRunPtr) && (kobj->ownerPtr != NULL))
    {
        if (timeout == RK_NO_WAIT)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_LOCK, RK_ERR_MUTEX_LOCKED,
                               kobj->waitingQueue.size);
            if (callNumber == RK_SYSCALL_MUTEX_LOCK)
            {
                kSyscallTaskClear(RK_gRunPtr);
            }
            RK_CR_EXIT
            return (RK_ERR_MUTEX_LOCKED);
        }

        if ((timeout != RK_WAIT_FOREVER) && (timeout > 0UL))
        {
            RK_TASK_TIMEOUT_WAITINGQUEUE_SETUP

            RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                RK_gRunPtr->timeoutNode.timeoutType = 0U;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                kTraceRecordObject(kobj, RK_TRACE_OP_LOCK, err,
                                   kobj->waitingQueue.size);
                if (callNumber == RK_SYSCALL_MUTEX_LOCK)
                {
                    kSyscallTaskClear(RK_gRunPtr);
                }
                RK_CR_EXIT
                return (err);
            }
        }
        if (timeout == RK_WAIT_FOREVER)
        {
            RK_gRunPtr->timeoutNode.waitingQueuePtr = &kobj->waitingQueue;
        }

        kTraceRecordObject(kobj, RK_TRACE_OP_LOCK_BLOCK, RK_ERR_SUCCESS,
                           kobj->waitingQueue.size + 1UL);
        kTCBQEnqByPrio(&kobj->waitingQueue, RK_gRunPtr);

        RK_gRunPtr->status = RK_BLOCKED;
        RK_gRunPtr->waitingForMutexPtr = kobj;
        if (kobj->protocol == RK_PRIO_INHERITANCE)
        {
            kMutexUpdateOwnerPrio_(kobj->ownerPtr);
        }

        kSyscallTaskSuspend(framePtr, callNumber, arg0, arg1, arg2, arg3,
                            phase);
        kPendCtxSwtch();
        RK_CR_EXIT
        return (RK_ERR_SYSCALL_RESTART);
    }

    if (kobj->ownerPtr == RK_gRunPtr)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_MUTEX_REC_LOCK);
#endif
        if (callNumber == RK_SYSCALL_MUTEX_LOCK)
        {
            kSyscallTaskClear(RK_gRunPtr);
        }
        RK_CR_EXIT
        return (RK_ERR_MUTEX_REC_LOCK);
    }

    kTraceRecordObject(kobj, RK_TRACE_OP_LOCK, RK_ERR_SUCCESS,
                       kobj->waitingQueue.size);
    if (callNumber == RK_SYSCALL_MUTEX_LOCK)
    {
        kSyscallTaskClear(RK_gRunPtr);
    }
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMutexLockSyscall(RK_EXCEPTION_FRAME *const framePtr,
                         RK_MUTEX_HANDLE const mutexHandle,
                         RK_TICK const timeout)
{
    return (kMutexLockSyscallContinue(
        framePtr, mutexHandle, timeout, RK_SYSCALL_MUTEX_LOCK,
        (ULONG)(UINTPTR)mutexHandle, (ULONG)timeout, 0UL, 0UL,
        RK_SYSCALL_PHASE_WAIT));
}

RK_ERR kMutexUnlock(RK_MUTEX_HANDLE const mutexHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MUTEX_UNLOCK,
                                        (ULONG)(UINTPTR)mutexHandle, 0UL, 0UL,
                                        0UL));
    }

    RK_MUTEX *kobj = NULL;
    RK_ERR const resolveErr = kMutexResolve_(mutexHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER
    RK_TCB *tcbPtr = NULL;

    RK_ERR const readyErr = kMutexReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        kMutexReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (kIsISR())
    {
        RK_CR_EXIT
        return (kMutexReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if ((kobj->ownerFaulted == RK_TRUE) && (kobj->lock == RK_FALSE))
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_UNLOCK,
                           RK_ERR_MUTEX_NOT_LOCKED,
                           kobj->waitingQueue.size);
        RK_CR_EXIT
        return (RK_ERR_MUTEX_NOT_LOCKED);
    }

    if (kobj->lock == RK_FALSE)
    {
        RK_CR_EXIT
        return (kMutexReportErr_(RK_ERR_MUTEX_NOT_LOCKED));
    }

    if (kobj->ownerPtr != RK_gRunPtr)
    {
        RK_CR_EXIT
        return (kMutexReportErr_(RK_ERR_MUTEX_NOT_OWNER));
    }

    kMutexListRem(&(RK_gRunPtr->ownedMutexList), &(kobj->mutexNode));

    if (kobj->waitingQueue.size == 0UL)
    {
        kobj->lock = RK_FALSE;
        kobj->ownerPtr = NULL;

        if (kobj->protocol == RK_PRIO_INHERITANCE)
        {
            kMutexUpdateOwnerPrio_(RK_gRunPtr);
            RK_BARRIER
        }

        kTraceRecordObject(kobj, RK_TRACE_OP_UNLOCK, RK_ERR_SUCCESS, 0UL);
    }
    else
    {
        /*
         * Ownership transfer is atomic with dequeuing the waiter: after this
         * block the mutex remains locked, the new owner has the mutex in its
         * owned list, and the old owner no longer contributes inheritance.
         */
        kTCBQDeq(&(kobj->waitingQueue), &tcbPtr);
        if (tcbPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
        {
            kRemoveTimeoutNode(&tcbPtr->timeoutNode);
            tcbPtr->timeoutNode.timeoutType = 0;
        }
        tcbPtr->timeoutNode.waitingQueuePtr = NULL;
        kobj->ownerPtr = tcbPtr;
        kMutexListAdd(&(tcbPtr->ownedMutexList), &(kobj->mutexNode));
        kobj->lock = RK_TRUE;
        tcbPtr->waitingForMutexPtr = NULL;

        if (kobj->protocol == RK_PRIO_INHERITANCE)
        {
            kMutexUpdateOwnerPrio_(RK_gRunPtr);
            kMutexUpdateOwnerPrio_(tcbPtr);
        }

        kTraceRecordObject(kobj, RK_TRACE_OP_UNLOCK, RK_ERR_SUCCESS,
                           kobj->waitingQueue.size);
        kReadySwtch(tcbPtr);
    }

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMutexQuery(RK_MUTEX_HANDLE const mutexHandle, UINT *const statePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MUTEX_QUERY,
                                        (ULONG)(UINTPTR)mutexHandle,
                                        (ULONG)(UINTPTR)statePtr, 0UL, 0UL));
    }

    RK_MUTEX *kobj = NULL;
    RK_ERR const resolveErr = kMutexResolve_(mutexHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMutexReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        kMutexReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (statePtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    *statePtr = ((UINT)kobj->lock);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

#endif /* mutex */
