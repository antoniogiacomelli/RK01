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
 *   Stateless wait queues plus condition-variable helpers. A sleep queue
 *   records blocked tasks only; the application or monitor owns the predicate.
 *
 * Contracts/invariants:
 *   - The queue does not store a condition value or a missed signal.
 *   - Every queued task has timeout metadata that points back to this queue
 *     when the wait is bounded.
 *   - Wake/signal removes tasks from the wait queue before making them ready.
 */

/**
 * @note
 * A Sleep Queue is a stateless wait queue: it does not test a predicate and
 * does not remember previous signals. kSleepQueueSleep() suspends the running
 * task until another task wakes it, signals it, or its timeout expires.
 * A RK_NO_WAIT is a NOP.
 *
 * The condition variable helpers use a Sleep Queue with a Mutex. The
 * monitor code owns the predicate and calls kCondVarWait() in a Mesa-style
 * loop while holding the mutex. One might use other signal disciplines by
 * composing SLPQs and MUTEXEs. The atomicity is for sleep+unlock is achieved
 * using kSchLock() in the helpers.
 *
 * @code
 * while (!condition)
 * {
 *     kCondVarWait(monitor->queueHandle, monitor->lockHandle,
 *                  TIMEOUT_IN_TICKS);
 * @note time-out covers both sleep queue and lock wait time
 * }
 *
 */


#define RK_SOURCE_CODE

#include <kdynobjs.h>
#include <ksleepq.h>
#include <ksch.h>
#include <ksystasks.h>
#include <ksyscall.h>
#include <ktimer.h>
#include <ktrace.h>

#if (RK_CONF_SLEEP_QUEUE == ON)
static RK_ERR kSleepQueueResolve_(RK_SLEEP_QUEUE_HANDLE const sleepqHandle,
                                  RK_SLEEP_QUEUE **const sleepqPPtr)
{
    if (sleepqPPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    *sleepqPPtr = NULL;

    VOID *objPtr = NULL;
    RK_ERR const err =
        kDynObjResolveHandle(RK_DYN_OBJ_TYPE_SLEEP_QUEUE, sleepqHandle,
                             &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
    *sleepqPPtr = (RK_SLEEP_QUEUE *)objPtr;
    return (RK_ERR_SUCCESS);
}

static RK_ERR kSleepQueueReadyErr_(RK_SLEEP_QUEUE const *const kobj)
{
    RK_ERR const err =
        kObjHeaderReadyErr((kobj != NULL) ? &kobj->header : NULL,
                           RK_SLEEPQ_KOBJ_ID);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    return (kObjHeaderDomainLocalAccessErr(
        &kobj->header, (RK_gRunPtr != NULL) ? RK_gRunPtr->domainPtr : NULL));
}

static RK_ERR kSleepQueueReportErr_(RK_ERR const err)
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

RK_ERR kSleepQueueInit(RK_SLEEP_QUEUE *const kobj)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SLEEP_QUEUE_INIT,
                                        (ULONG)(UINTPTR)kobj,
                                        0UL, 0UL, 0UL));
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

    RK_ERR const err = kTCBQInit(&(kobj->waitingQueue));
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    kobj->init = RK_TRUE;
    kobj->objID = RK_SLEEPQ_KOBJ_ID;
    kobj->objName[0] = '\0';
    kObjHeaderOwnerDomainSet(&kobj->header,
                             (RK_gRunPtr != NULL) ? RK_gRunPtr->domainPtr
                                                  : NULL);
    kTraceRegisterObject(kobj, RK_SLEEPQ_KOBJ_ID);

    RK_CR_EXIT

    return (RK_ERR_SUCCESS);
}

RK_ERR kSleepQueueSleep(RK_SLEEP_QUEUE_HANDLE const sleepqHandle,
                        RK_TICK const timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SLEEP_QUEUE_SLEEP, (ULONG)(UINTPTR)sleepqHandle,
            (ULONG)timeout, 0UL, 0UL));
    }

    RK_SLEEP_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kSleepQueueResolve_(sleepqHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSleepQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSleepQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (kIsISR())
    {
        RK_CR_EXIT
        return (kSleepQueueReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (timeout == RK_NO_WAIT)
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_WAIT, RK_ERR_NOWAIT,
                           kobj->waitingQueue.size);
        RK_CR_EXIT
        return (RK_ERR_NOWAIT);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > 0))
    {
        RK_TASK_TIMEOUT_WAITINGQUEUE_SETUP

        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            RK_gRunPtr->timeoutNode.timeoutType = 0;
            RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
            RK_CR_EXIT
            return (err);
        }
    }
    RK_gRunPtr->status = RK_SLEEPING;
    kTraceRecordObject(kobj, RK_TRACE_OP_WAIT_BLOCK, RK_ERR_SUCCESS,
                       kobj->waitingQueue.size + 1UL);
    kTCBQEnqByPrio(&kobj->waitingQueue, RK_gRunPtr);

    kPendCtxSwtch();
    RK_CR_EXIT
    /* resuming here, if time is out, return error */
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

    kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, RK_ERR_SUCCESS,
                       kobj->waitingQueue.size);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSleepQueueSleepSyscall(RK_EXCEPTION_FRAME *const framePtr,
                               RK_SLEEP_QUEUE_HANDLE const sleepqHandle,
                               RK_TICK const timeout)
{
    RK_SLEEP_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kSleepQueueResolve_(sleepqHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSleepQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSleepQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (timeout == RK_NO_WAIT)
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_WAIT, RK_ERR_NOWAIT,
                           kobj->waitingQueue.size);
        kSyscallTaskClear(RK_gRunPtr);
        RK_CR_EXIT
        return (RK_ERR_NOWAIT);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > 0UL))
    {
        RK_TASK_TIMEOUT_WAITINGQUEUE_SETUP

        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            RK_gRunPtr->timeoutNode.timeoutType = 0U;
            RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
            kSyscallTaskClear(RK_gRunPtr);
            RK_CR_EXIT
            return (err);
        }
    }

    RK_gRunPtr->status = RK_SLEEPING;
    kTraceRecordObject(kobj, RK_TRACE_OP_WAIT_BLOCK, RK_ERR_SUCCESS,
                       kobj->waitingQueue.size + 1UL);
    kTCBQEnqByPrio(&kobj->waitingQueue, RK_gRunPtr);
    kSyscallTaskSuspend(framePtr, RK_SYSCALL_SLEEP_QUEUE_SLEEP,
                        (ULONG)(UINTPTR)sleepqHandle, (ULONG)timeout, 0UL,
                        0UL, RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

RK_ERR kSleepQueueSignal(RK_SLEEP_QUEUE_HANDLE const sleepqHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SLEEP_QUEUE_SIGNAL, (ULONG)(UINTPTR)sleepqHandle, 0UL,
            0UL, 0UL));
    }

    RK_SLEEP_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kSleepQueueResolve_(sleepqHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSleepQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSleepQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (kobj->waitingQueue.size == 0)
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_WAKE,
                           RK_ERR_EMPTY_WAITING_QUEUE, 0UL);
        RK_CR_EXIT
        return (RK_ERR_EMPTY_WAITING_QUEUE);
    }

    RK_TCB *nextTCBPtr = NULL;

    kTCBQDeq(&kobj->waitingQueue, &nextTCBPtr);
    if (nextTCBPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
    {
        kRemoveTimeoutNode(&nextTCBPtr->timeoutNode);
        nextTCBPtr->timeoutNode.timeoutType = 0;
        nextTCBPtr->timeoutNode.waitingQueuePtr = NULL;
    }

    kReadySwtch(nextTCBPtr);
    kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, RK_ERR_SUCCESS,
                       kobj->waitingQueue.size);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

/* cherry pick a task to wake*/
RK_ERR kSleepQueueReady(RK_SLEEP_QUEUE_HANDLE const sleepqHandle,
                        RK_TASK_HANDLE taskHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SLEEP_QUEUE_READY, (ULONG)(UINTPTR)sleepqHandle,
            (ULONG)(UINTPTR)taskHandle, 0UL, 0UL));
    }

    RK_SLEEP_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kSleepQueueResolve_(sleepqHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSleepQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSleepQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (kobj->waitingQueue.size == 0)
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_WAKE,
                           RK_ERR_EMPTY_WAITING_QUEUE, 0UL);
        RK_CR_EXIT
        return (RK_ERR_EMPTY_WAITING_QUEUE);
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR err = kTaskHandleResolve(taskHandle, &taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)err);
#endif
        RK_CR_EXIT
        return (err);
    }

    err = kTCBQRem(&kobj->waitingQueue, &taskPtr);

    K_ASSERT(err == RK_ERR_SUCCESS);

    if (taskPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
    {
        kRemoveTimeoutNode(&taskPtr->timeoutNode);
        taskPtr->timeoutNode.timeoutType = 0;
        taskPtr->timeoutNode.waitingQueuePtr = NULL;
    }

    kReadySwtch(taskPtr);
    kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, RK_ERR_SUCCESS,
                       kobj->waitingQueue.size);

    RK_CR_EXIT

    return (RK_ERR_SUCCESS);
}

RK_ERR kSleepQueueQuery(RK_SLEEP_QUEUE_HANDLE const sleepqHandle,
                        ULONG *const nTasksPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SLEEP_QUEUE_QUERY, (ULONG)(UINTPTR)sleepqHandle,
            (ULONG)(UINTPTR)nTasksPtr, 0UL, 0UL));
    }

    RK_SLEEP_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kSleepQueueResolve_(sleepqHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSleepQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSleepQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (nTasksPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    *nTasksPtr = kobj->waitingQueue.size;
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSleepQueueWake(RK_SLEEP_QUEUE_HANDLE const sleepqHandle, UINT nTasks,
                       UINT *uTasksPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SLEEP_QUEUE_WAKE, (ULONG)(UINTPTR)sleepqHandle,
            (ULONG)nTasks, (ULONG)(UINTPTR)uTasksPtr, 0UL));
    }

    RK_SLEEP_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kSleepQueueResolve_(sleepqHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSleepQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSleepQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    UINT nWaiting = kobj->waitingQueue.size;

    if (nWaiting == 0)
    {
        if (uTasksPtr)
            *uTasksPtr = 0;
        kTraceRecordObject(kobj, RK_TRACE_OP_WAKE,
                           RK_ERR_EMPTY_WAITING_QUEUE, 0UL);
        RK_CR_EXIT
        return (RK_ERR_EMPTY_WAITING_QUEUE);
    }

    /* Wake up to nTasks, but no more than nWaiting */
    UINT toWake = 0;
    if (nTasks == 0)
    {
        /* if 0, wake'em all */
        toWake = nWaiting;
    }
    else
    {
        toWake = (nTasks < nWaiting) ? (nTasks) : (nWaiting);
    }

    if (kIsISR())
    {
        if (uTasksPtr != NULL)
        {
#if (RK_CONF_ERR_CHECK == ON)
            K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
            RK_CR_EXIT
            return (RK_ERR_INVALID_PARAM);
        }
        kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, RK_ERR_SUCCESS,
                           (ULONG)toWake);
        RK_CR_EXIT
        return (
            kPostProcJobEnq(RK_POSTPROC_JOB_SLEEPQ_WAKE, (VOID *)kobj, toWake));
    }

    RK_CR_EXIT

    kSchLock();

    RK_TCB *chosenTCBPtr = NULL;
    RK_ERR ret = RK_ERR_SUCCESS;

    for (UINT i = 0U; i < toWake; i++)
    {
        RK_CR_ENTER
        if (kobj->waitingQueue.size == 0U)
        {
            RK_CR_EXIT
            break;
        }

        RK_TCB *nextTCBPtr = NULL;
        ret = kTCBQDeq(&kobj->waitingQueue, &nextTCBPtr);
        if (ret != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            break;
        }
        if (nextTCBPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
        {
            kRemoveTimeoutNode(&nextTCBPtr->timeoutNode);
            nextTCBPtr->timeoutNode.timeoutType = 0;
            nextTCBPtr->timeoutNode.waitingQueuePtr = NULL;
        }
        ret = kReadyNoSwtch(nextTCBPtr);
        if (ret != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            break;
        }
        if ((chosenTCBPtr == NULL) ||
            (nextTCBPtr->priority < chosenTCBPtr->priority))
        {
            chosenTCBPtr = nextTCBPtr;
        }

        RK_CR_EXIT
    }

    RK_CR_ENTER
    if (uTasksPtr)
    {
        *uTasksPtr = (UINT)kobj->waitingQueue.size;
    }
    kTraceRecordObject(kobj, RK_TRACE_OP_WAKE, ret,
                       (ULONG)(nWaiting - kobj->waitingQueue.size));
    if (chosenTCBPtr != NULL)
    {
        kReschedTask(chosenTCBPtr);
    }
    RK_CR_EXIT

    kSchUnlock();
    return (ret);
}

RK_ERR kSleepQueueUnready(RK_SLEEP_QUEUE_HANDLE const sleepqHandle,
                          RK_TASK_HANDLE handle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SLEEP_QUEUE_UNREADY, (ULONG)(UINTPTR)sleepqHandle,
            (ULONG)(UINTPTR)handle, 0UL, 0UL));
    }

    RK_SLEEP_QUEUE *kobj = NULL;
    RK_ERR const resolveErr = kSleepQueueResolve_(sleepqHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kSleepQueueReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kSleepQueueReportErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR taskResolveErr = kTaskHandleResolve(handle, &taskPtr);
    if (taskResolveErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (taskResolveErr);
    }

    if ((taskPtr == RK_gRunPtr) || (taskPtr->status != RK_READY))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    RK_TCB *taskRemPtr = taskPtr;
    kTCBQRem(&RK_gReadyQueue[taskPtr->priority], &taskRemPtr);
    RK_ERR err = kTCBQEnqByPrio(&kobj->waitingQueue, taskPtr);
    if (!err)
    {
        taskPtr->status = RK_SLEEPQ_BLOCKED;
    }
    kTraceRecordObject(kobj, RK_TRACE_OP_BLOCK, err, kobj->waitingQueue.size);
    RK_CR_EXIT
    return (err);
}
#if (RK_CONF_CONDVAR == ON)

RK_ERR kCondVarInit(RK_SLEEP_QUEUE *const cond, RK_MUTEX *const lock)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_CONDVAR_INIT,
                                        (ULONG)(UINTPTR)cond,
                                        (ULONG)(UINTPTR)lock, 0UL, 0UL));
    }

    RK_ERR const phaseErr = kKernelRawInitGuard();
    if (phaseErr != RK_ERR_SUCCESS)
        return (phaseErr);

    RK_ERR err = kSleepQueueInit(cond);
    if (err != RK_ERR_SUCCESS)
        return (err);
    err = kMutexInit(lock, RK_PRIO_INHERITANCE);
    return (err);
}

RK_ERR kCondVarWait(RK_SLEEP_QUEUE_HANDLE const cond,
                    RK_MUTEX_HANDLE const lock,
                    RK_TICK timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_CONDVAR_WAIT, (ULONG)(UINTPTR)cond,
            (ULONG)(UINTPTR)lock, (ULONG)timeout, 0UL));
    }

    RK_SLEEP_QUEUE *condPtr = NULL;
    RK_ERR const condResolveErr = kSleepQueueResolve_(cond, &condPtr);
    if (condResolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)condResolveErr);
#endif
        return (condResolveErr);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    /*
     * RK_NO_WAIT cannot express a condition variable wait.
     * Finite timeouts must remain within the wrap-safe half-range.
     */
    if ((timeout == RK_NO_WAIT) ||
        ((timeout != RK_WAIT_FOREVER) &&
         (timeout > RK_MAX_PERIOD)))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
#endif
        return (RK_ERR_INVALID_TIMEOUT);
    }

    RK_TICK deadline = 0UL;

    if (timeout != RK_WAIT_FOREVER)
    {
        deadline = K_TICK_ADD(kTickGet(), timeout);
    }

    {
        RK_CR_AREA
        RK_CR_ENTER
        RK_ERR const readyErr = kSleepQueueReadyErr_(condPtr);
        if (readyErr != RK_ERR_SUCCESS)
        {
            (VOID)kSleepQueueReportErr_(readyErr);
            RK_CR_EXIT
            return (readyErr);
        }
        RK_CR_EXIT
    }

    /*
     * prevent dispatch between releasing the Mutex and entering the
     * cond queue.
     */
    kPreemptDisable();

    RK_ERR const unlockErr = kMutexUnlock(lock);

    if (unlockErr != RK_ERR_SUCCESS)
    {
        kPreemptEnable();
        return (unlockErr);
    }

    RK_ERR waitErr;

    if (timeout == RK_WAIT_FOREVER)
    {
        waitErr = kSleepQueueSleep(cond, RK_WAIT_FOREVER);
    }
    else
    {
        RK_STICK const remaining =
            K_TICK_DIFF(deadline, kTickGet());

        if (remaining > 0)
        {
            waitErr = kSleepQueueSleep(cond, (RK_TICK)remaining);
        }
        else
        {
            /*
             * not call kSleepQueueSleep() with RK_NO_WAIT.
             * The Mutex must still be reacquired below.
             */
            waitErr = RK_ERR_TIMEOUT;
        }
    }

    /*
     * reacquire while the scheduler lock is still owned. If the Mutex
     * is held, this call blocks normally; other tasks execute with their
     * own scheduler-lock state.
     */
    RK_ERR const lockErr =
        kMutexLock(lock, RK_WAIT_FOREVER);

    kPreemptEnable();

    if (waitErr == RK_ERR_TIMEOUT)
    {
        K_PANIC("Condition variable timed out");
    }

    return ((lockErr != RK_ERR_SUCCESS) ? lockErr : waitErr);
}

RK_ERR kCondVarWaitSyscall(RK_EXCEPTION_FRAME *const framePtr,
                           RK_SLEEP_QUEUE_HANDLE const condHandle,
                           RK_MUTEX_HANDLE const lockHandle,
                           RK_TICK const timeout)
{
    RK_TICK deadline = 0UL;
    RK_SLEEP_QUEUE *cond = NULL;

#if (RK_CONF_ERR_CHECK == ON)
    if ((timeout == RK_NO_WAIT) ||
        ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD)))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }
#endif

    if ((timeout == RK_NO_WAIT) ||
        ((timeout != RK_WAIT_FOREVER) && (timeout > RK_MAX_PERIOD)))
    {
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_TIMEOUT);
    }

    if ((RK_gRunPtr != NULL) &&
        (RK_gRunPtr->syscallNumber == RK_SYSCALL_CONDVAR_WAIT) &&
        (RK_gRunPtr->syscallPhase == RK_SYSCALL_PHASE_CONDVAR_RELOCK))
    {
        deadline = (RK_TICK)RK_gRunPtr->syscallArg3;
        K_UNUSE(deadline);
    }
    else
    {
        RK_ERR const condResolveErr =
            kSleepQueueResolve_(condHandle, &cond);
        if (condResolveErr != RK_ERR_SUCCESS)
        {
#if (RK_CONF_ERR_CHECK == ON)
            K_ERR_HANDLER((RK_FAULT)condResolveErr);
#endif
            kSyscallTaskClear(RK_gRunPtr);
            return (condResolveErr);
        }

        if (timeout != RK_WAIT_FOREVER)
        {
            deadline = K_TICK_ADD(kTickGet(), timeout);
        }

        kPreemptDisable();

        RK_ERR const unlockErr = kMutexUnlock(lockHandle);
        if (unlockErr != RK_ERR_SUCCESS)
        {
            kPreemptEnable();
            kSyscallTaskClear(RK_gRunPtr);
            return (unlockErr);
        }

        RK_TICK waitTicks = timeout;
        if (timeout != RK_WAIT_FOREVER)
        {
            RK_STICK const remaining = K_TICK_DIFF(deadline, kTickGet());
            if (remaining <= 0)
            {
                kSyscallTaskSuspend(framePtr, RK_SYSCALL_CONDVAR_WAIT,
                                    (ULONG)(UINTPTR)condHandle,
                                    (ULONG)(UINTPTR)lockHandle,
                                    (ULONG)timeout,
                                    (ULONG)deadline,
                                    RK_SYSCALL_PHASE_CONDVAR_RELOCK);
                RK_gRunPtr->syscallWakeResult = RK_ERR_TIMEOUT;
                goto relock;
            }
            waitTicks = (RK_TICK)remaining;
        }

        {
            RK_CR_AREA
            RK_CR_ENTER

            RK_ERR const readyErr = kSleepQueueReadyErr_(cond);
            if (readyErr != RK_ERR_SUCCESS)
            {
                (VOID)kSleepQueueReportErr_(readyErr);
                RK_CR_EXIT
                kPreemptEnable();
                kSyscallTaskClear(RK_gRunPtr);
                return (readyErr);
            }

            if ((waitTicks != RK_WAIT_FOREVER) && (waitTicks > 0UL))
            {
                RK_gRunPtr->timeoutNode.timeoutType = RK_TIMEOUT_BLOCKING;
                RK_gRunPtr->timeoutNode.waitingQueuePtr =
                    &cond->waitingQueue;
                RK_BARRIER

                RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode,
                                             waitTicks);
                if (err != RK_ERR_SUCCESS)
                {
                    RK_gRunPtr->timeoutNode.timeoutType = 0U;
                    RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
                    RK_CR_EXIT
                    kPreemptEnable();
                    kSyscallTaskClear(RK_gRunPtr);
                    return (err);
                }
            }

            RK_gRunPtr->status = RK_SLEEPING;
            kTraceRecordObject(cond, RK_TRACE_OP_WAIT_BLOCK, RK_ERR_SUCCESS,
                               cond->waitingQueue.size + 1UL);
            kTCBQEnqByPrio(&cond->waitingQueue, RK_gRunPtr);
            kSyscallTaskSuspend(framePtr, RK_SYSCALL_CONDVAR_WAIT,
                                (ULONG)(UINTPTR)condHandle,
                                (ULONG)(UINTPTR)lockHandle, (ULONG)timeout,
                                (ULONG)deadline, RK_SYSCALL_PHASE_WAIT);
            kPendCtxSwtch();
            RK_CR_EXIT
            return (RK_ERR_SYSCALL_RESTART);
        }
    }

relock:
    {
        RK_ERR const lockErr = kMutexLockSyscallContinue(
            framePtr, lockHandle, RK_WAIT_FOREVER, RK_SYSCALL_CONDVAR_WAIT,
            (ULONG)(UINTPTR)condHandle, (ULONG)(UINTPTR)lockHandle,
            (ULONG)timeout, (ULONG)deadline,
            RK_SYSCALL_PHASE_CONDVAR_RELOCK);

        if (lockErr == RK_ERR_SYSCALL_RESTART)
        {
            return (lockErr);
        }

        kPreemptEnable();

        RK_ERR const waitErr =
            (RK_gRunPtr != NULL) ? RK_gRunPtr->syscallWakeResult
                                 : RK_ERR_ERROR;

        kSyscallTaskClear(RK_gRunPtr);

        if (waitErr == RK_ERR_TIMEOUT)
        {
            K_PANIC("Condition variable timed out");
        }

        return ((lockErr != RK_ERR_SUCCESS) ? lockErr : waitErr);
    }
}

RK_ERR kCondVarSignal(RK_SLEEP_QUEUE_HANDLE const cond)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_CONDVAR_SIGNAL, (ULONG)(UINTPTR)cond, 0UL, 0UL,
            0UL));
    }

    return (kSleepQueueSignal(cond));
}

RK_ERR kCondVarBroadcast(RK_SLEEP_QUEUE_HANDLE const cond)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_CONDVAR_BROADCAST, (ULONG)(UINTPTR)cond, 0UL,
            0UL, 0UL));
    }

    return (kSleepQueueWake(cond, 0U, NULL));
}
#endif

#endif /* RK_CONF_SLEEP_QUEUE */
