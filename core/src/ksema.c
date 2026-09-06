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
 *   Counting/binary semaphore implementation. Pool-created semaphores use
 *   encoded handles and kernel wait queues.
 *
 * Contracts/invariants:
 *   - value is always in the inclusive range [0, maxValue].
 *   - waitingQueue contains only tasks blocked on this semaphore.
 *   - kSemaphorePend()/kSemaphorePost() never spin in user mode.
 */

#define RK_SOURCE_CODE

#include <kdynobjs.h>
#include <ksema.h>
#include <ksch.h>
#include <ksyscall.h>
#include <ktrace.h>

#if (RK_CONF_SEMAPHORE == ON)
/******************************************************************************/
/* COUNTING/BIN SEMAPHORES                                                    */
/******************************************************************************/
/******************************************************************************/
/* A semaphore has a maxValue. To create a binary semaphore set its max value */
/* to 1U. Note, when signalling a semaphore whose value reached its limit,    */
/* return code is not a FAULT (negative) but a positive value                 */
/* (RK_ERR_SEMA_FULL).                                                        */
/* Depending on the case it might or not mean an error in the synch logic.    */
/******************************************************************************/

#ifndef K_SEMA_IS_BINARY
#define K_SEMA_IS_BINARY(kobj) ((kobj)->maxValue == 1U)
#endif

static inline UINT kSemaphoreValueLoad_(RK_SEMAPHORE const *const kobj)
{
    return (kAtomicLoadU32(&kobj->value));
}

static inline VOID kSemaphoreValueStore_(RK_SEMAPHORE *const kobj,
                                         UINT const value)
{
    kAtomicStoreU32(&kobj->value, value);
}

static inline RK_ERR kSemaphorePublicReadyErr_(RK_ERR const err)
{
    if ((err == RK_ERR_RESCHED_PENDING) ||
        (err == RK_ERR_RESCHED_NOT_NEEDED))
    {
        return (RK_ERR_SUCCESS);
    }

    return (err);
}

static RK_ERR kSemaphoreResolve_(RK_SEMAPHORE_HANDLE const semaHandle,
                                 RK_SEMAPHORE **const semaPPtr)
{
    if (semaPPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    *semaPPtr = NULL;

    VOID *objPtr = NULL;
    RK_ERR const err =
        kDynObjResolveHandle(RK_DYN_OBJ_TYPE_SEMAPHORE, semaHandle,
                             &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
    *semaPPtr = (RK_SEMAPHORE *)objPtr;
    return (RK_ERR_SUCCESS);
}

static RK_ERR kSemaphoreReadyErr_(RK_SEMAPHORE const *const kobj)
{
    RK_ERR const err =
        kObjHeaderReadyErr((kobj != NULL) ? &kobj->header : NULL,
                           RK_SEMAPHORE_KOBJ_ID);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    return (kObjHeaderModuleLocalAccessErr(
        &kobj->header, (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr : NULL));
}

static RK_ERR kSemaphoreReportErr_(RK_ERR const err)
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
#else
    (VOID)err;
#endif
    return (err);
}

RK_ERR kSemaphoreInit(RK_SEMAPHORE *const kobj,
                      const UINT initValue, const UINT maxValue)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SEMAPHORE_INIT,
                                        (ULONG)(UINTPTR)kobj,
                                        (ULONG)initValue,
                                        (ULONG)maxValue, 0UL));
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
    if ((maxValue == 0U) || (initValue > maxValue))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    RK_ERR err = kTCBQInit(&(kobj->waitingQueue));
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    kobj->init = RK_TRUE;
    kobj->objID = RK_SEMAPHORE_KOBJ_ID;
    kobj->objName[0] = '\0';
    kObjHeaderOwnerModuleSet(&kobj->header,
                             (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr
                                                  : NULL);
    kobj->maxValue = maxValue;
    kSemaphoreValueStore_(kobj, initValue);
    kTraceRegisterObject(kobj, RK_SEMAPHORE_KOBJ_ID);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSemaphorePend(RK_SEMAPHORE_HANDLE const semaHandle,
                      const RK_TICK timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SEMAPHORE_PEND, (ULONG)(UINTPTR)semaHandle,
            (ULONG)timeout, 0UL, 0UL));
    }

    RK_SEMAPHORE *kobj = NULL;
    RK_ERR const resolveErr = kSemaphoreResolve_(semaHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSemaphoreReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSemaphoreReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (K_BLOCKING_ON_ISR(timeout))
    {
        RK_CR_EXIT
        return (kSemaphoreReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (kSemaphoreValueLoad_(kobj) > 0U)
    {
        if (K_SEMA_IS_BINARY(kobj))
        {
            kSemaphoreValueStore_(kobj, 0U);
        }
        else
        {
            kobj->value = kobj->value - 1U;
        }
        kTraceRecordObject(kobj, RK_TRACE_OP_PEND, RK_ERR_SUCCESS,
                           kobj->value);
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }
    else
    {

        if (timeout == RK_NO_WAIT)
        {
            kTraceRecordObject(kobj, RK_TRACE_OP_PEND, RK_ERR_SEMA_BLOCKED,
                               kobj->waitingQueue.size);
            RK_CR_EXIT
            return (RK_ERR_SEMA_BLOCKED);
        }
        if ((timeout != RK_WAIT_FOREVER) && (timeout > 0))
        {
            RK_TASK_TIMEOUT_WAITINGQUEUE_SETUP

            RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
            if (err != RK_ERR_SUCCESS)
            {
                RK_gRunPtr->timeoutNode.timeoutType = 0;
                RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                kTraceRecordObject(kobj, RK_TRACE_OP_PEND, err,
                                   kobj->waitingQueue.size);
                RK_CR_EXIT
                return (err);
            }
        }
        RK_gRunPtr->status = RK_BLOCKED;
        kTraceRecordObject(kobj, RK_TRACE_OP_PEND_BLOCK, RK_ERR_SUCCESS,
                           kobj->waitingQueue.size + 1UL);
        kTCBQEnqByPrio(&kobj->waitingQueue, RK_gRunPtr);
        kPendCtxSwtch();
        RK_CR_EXIT
        RK_CR_ENTER
        if (RK_gRunPtr->timeOut)
        {
            RK_gRunPtr->timeOut = RK_FALSE;
            kTraceRecordObject(kobj, RK_TRACE_OP_TIMEOUT, RK_ERR_TIMEOUT,
                               kobj->waitingQueue.size);
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
    kTraceRecordObject(kobj, RK_TRACE_OP_PEND, RK_ERR_SUCCESS, kobj->value);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSemaphorePendSyscall(RK_EXCEPTION_FRAME *const framePtr,
                             RK_SEMAPHORE_HANDLE const semaHandle,
                             RK_TICK const timeout)
{
    RK_SEMAPHORE *kobj = NULL;
    RK_ERR const resolveErr = kSemaphoreResolve_(semaHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSemaphoreReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSemaphoreReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (kSemaphoreValueLoad_(kobj) > 0U)
    {
        if (K_SEMA_IS_BINARY(kobj))
        {
            kSemaphoreValueStore_(kobj, 0U);
        }
        else
        {
            kobj->value = kobj->value - 1U;
        }
        kTraceRecordObject(kobj, RK_TRACE_OP_PEND, RK_ERR_SUCCESS,
                           kobj->value);
        kSyscallTaskClear(RK_gRunPtr);
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    if (timeout == RK_NO_WAIT)
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_PEND, RK_ERR_SEMA_BLOCKED,
                           kobj->waitingQueue.size);
        kSyscallTaskClear(RK_gRunPtr);
        RK_CR_EXIT
        return (RK_ERR_SEMA_BLOCKED);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > 0UL))
    {
        RK_TASK_TIMEOUT_WAITINGQUEUE_SETUP

        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            RK_gRunPtr->timeoutNode.timeoutType = 0U;
            RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
            kTraceRecordObject(kobj, RK_TRACE_OP_PEND, err,
                               kobj->waitingQueue.size);
            kSyscallTaskClear(RK_gRunPtr);
            RK_CR_EXIT
            return (err);
        }
    }
    else
    {
        RK_gRunPtr->timeoutNode.waitingQueuePtr = &kobj->waitingQueue;
    }

    RK_gRunPtr->status = RK_BLOCKED;
    kTraceRecordObject(kobj, RK_TRACE_OP_PEND_BLOCK, RK_ERR_SUCCESS,
                       kobj->waitingQueue.size + 1UL);
    kTCBQEnqByPrio(&kobj->waitingQueue, RK_gRunPtr);
    kSyscallTaskSuspend(framePtr, RK_SYSCALL_SEMAPHORE_PEND,
                        (ULONG)(UINTPTR)semaHandle, (ULONG)timeout, 0UL, 0UL,
                        RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

RK_ERR kSemaphorePost(RK_SEMAPHORE_HANDLE const semaHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SEMAPHORE_POST, (ULONG)(UINTPTR)semaHandle, 0UL, 0UL,
            0UL));
    }

    RK_SEMAPHORE *kobj = NULL;
    RK_ERR const resolveErr = kSemaphoreResolve_(semaHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSemaphoreReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSemaphoreReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    RK_TCB *nextTCBPtr = NULL;
    RK_ERR ret = -1;
    if (kobj->waitingQueue.size > 0)
    {
        kTCBQDeq(&(kobj->waitingQueue), &nextTCBPtr);
        if (nextTCBPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
        {
            kRemoveTimeoutNode(&nextTCBPtr->timeoutNode);
            nextTCBPtr->timeoutNode.timeoutType = 0;
            nextTCBPtr->timeoutNode.waitingQueuePtr = NULL;
        }
        ret = kSemaphorePublicReadyErr_(kReadySwtch(nextTCBPtr));
        kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, ret,
                           kobj->waitingQueue.size);
    }
    else
    {
        /* there are no waiting tasks */
        if (K_SEMA_IS_BINARY(kobj))
        {
            if (kSemaphoreValueLoad_(kobj) != 0U)
            {
                ret = RK_ERR_SEMA_FULL;
            }
            else
            {
                kSemaphoreValueStore_(kobj, 1U);
                ret = RK_ERR_SUCCESS;
            }
        }
        else
        {
            if (kobj->value == kobj->maxValue)
            {
                ret = RK_ERR_SEMA_FULL;
            }
            else
            {
                kobj->value += 1U;
                ret = RK_ERR_SUCCESS;
            }
        }
        kTraceRecordObject(kobj, RK_TRACE_OP_POST, ret, kobj->value);
    }
    RK_CR_EXIT
    return (ret);
}

RK_ERR kSemaphoreQuery(RK_SEMAPHORE_HANDLE const semaHandle,
                       INT *const countPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SEMAPHORE_QUERY, (ULONG)(UINTPTR)semaHandle,
            (ULONG)(UINTPTR)countPtr, 0UL, 0UL));
    }

    RK_SEMAPHORE *kobj = NULL;
    RK_ERR const resolveErr = kSemaphoreResolve_(semaHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSemaphoreReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSemaphoreReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (countPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->waitingQueue.size > 0)
    {
        INT retVal = (-((INT)kobj->waitingQueue.size));
        *countPtr = retVal;
    }
    else
    {
        *countPtr = (INT)kobj->value;
    }
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

#endif
