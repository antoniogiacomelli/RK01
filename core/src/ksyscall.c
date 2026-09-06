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
 *   SVC dispatcher and syscall continuation machinery. It snapshots user
 *   arguments, validates memory contracts, restarts blocking services and
 *   writes final results back to the stacked exception frame.
 */

#define RK_SOURCE_CODE

#include <kconsole.h>
#include <ksyscall.h>
#include <kdynobjs.h>
#include <kerr.h>
#include <kmem.h>
#include <kmesg.h>
#include <kmesgq.h>
#include <kmrm.h>
#include <kmutex.h>
#include <ksch.h>
#include <ksema.h>
#include <ksharedmem.h>
#include <ksleepq.h>
#include <ksynchmesg.h>
#include <ksysmon.h>
#include <ktaskevents.h>
#include <ktimer.h>
#include <ktrace.h>
#include <kstring.h>
#include <kversion.h>

/*
 * Syscall model
 * -------------
 * Public k* APIs run directly when called from privileged code. When the same
 * API is called from an unprivileged thread, the public wrapper calls
 * kSyscallInvoke4(), which enters SVC and lands here in privileged handler
 * mode.
 *
 * Blocking syscalls use a restart protocol. The service stores the original
 * call number and arguments in the running TCB, puts the task on the wait
 * queue, and returns RK_ERR_SYSCALL_RESTART to the user wrapper. The inline
 * wrapper loops on that value. When the task is made ready again, the saved
 * exception frame receives either the final return value or another restart,
 * so the same public API call can continue without exposing kernel state to
 * unprivileged code.
 *
 * Long non-blocking syscalls may use kSyscallTaskCheckpoint() at bounded
 * checkpoints. That stores the remaining work as replacement syscall arguments,
 * returns RK_ERR_SYSCALL_RESTART to user mode, and lets a pending PendSV run
 * before the wrapper re-enters SVC.
 *
 * Contracts/invariants:
 *   - User pointers are validated before privileged code dereferences them.
 *   - Packed syscall structs are copied once into kernel-local storage before
 *     any nested pointer is used.
 *   - Only continuation helpers write restart/final status for blocked
 *     syscalls; the dispatcher writes direct returns only.
 */
volatile unsigned RK_gSyscallThreadModeActive = 0U;

#if (RK_CONF_SVC_DEFER_TEST == ON)
#ifndef RK_SYSCALL_TEST_SPIN_CHUNK
#define RK_SYSCALL_TEST_SPIN_CHUNK (1024UL)
#endif

static volatile ULONG RK_gSyscallTestSpinSink;

static RK_ERR kSyscallTestSysTickDefer_(RK_EXCEPTION_FRAME *const framePtr,
                                        ULONG const iterations);
#endif

/* Optional core services may live outside this dispatcher unit. */
RK_FUNC_WEAK
RK_BOOL kLoggerSyscallDispatch(RK_EXCEPTION_FRAME *const framePtr,
                               ULONG const callNumber,
                               ULONG const arg0,
                               ULONG const arg1,
                               ULONG const arg2,
                               ULONG const arg3)
{
    K_UNUSE(framePtr);
    K_UNUSE(callNumber);
    K_UNUSE(arg0);
    K_UNUSE(arg1);
    K_UNUSE(arg2);
    K_UNUSE(arg3);
    return (RK_FALSE);
}

/* Applications may add private syscall numbers without replacing the kernel
 * dispatcher. The weak default says "not mine". */
RK_FUNC_WEAK
RK_BOOL kSyscallDispatchApp(RK_EXCEPTION_FRAME *const framePtr,
                            ULONG const callNumber,
                            ULONG const arg0,
                            ULONG const arg1,
                            ULONG const arg2,
                            ULONG const arg3)
{
    K_UNUSE(framePtr);
    K_UNUSE(callNumber);
    K_UNUSE(arg0);
    K_UNUSE(arg1);
    K_UNUSE(arg2);
    K_UNUSE(arg3);
    return (RK_FALSE);
}

static VOID kSyscallSetFrameReturn_(RK_EXCEPTION_FRAME *const framePtr,
                                    RK_ERR const ret)
{
    /* R0 is the normal ARM EABI return register and is also the stacked R0 in
     * the exception frame that will be restored on SVC return. */
    if (framePtr != NULL)
    {
        framePtr->r0 = (ULONG)ret;
    }
}

/* Overflow-safe byte-count helper for count * element-size validations. */
static RK_ERR kSyscallSizeMul_(ULONG const count,
                               ULONG const bytesEach,
                               ULONG *const bytesPtr)
{
    if (bytesPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if ((count != 0UL) && (bytesEach > (RK_ULONG_MAX / count)))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    *bytesPtr = count * bytesEach;
    return (RK_ERR_SUCCESS);
}

/* Required user input range: NULL is a caller error, not a zero-length range. */
static RK_ERR kSyscallUserReadRequired_(VOID const *const ptr,
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

/* Required user output range: kernel RAM and Flash must never be writable. */
static RK_ERR kSyscallUserWriteRequired_(VOID *const ptr,
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

static RK_ERR kSyscallRawHandleReadRequired_(RK_HANDLE const handle,
                                             ULONG const bytes)
{
    if (kDynObjHandleIsEncoded(handle) == RK_TRUE)
    {
        return (RK_ERR_SUCCESS);
    }

    return (kSyscallUserReadRequired_((VOID const *)(UINTPTR)handle, bytes));
}

static RK_ERR kSyscallRawHandleWriteRequired_(RK_HANDLE const handle,
                                              ULONG const bytes)
{
    if (kDynObjHandleIsEncoded(handle) == RK_TRUE)
    {
        return (RK_ERR_SUCCESS);
    }

    return (kSyscallUserWriteRequired_((VOID *)(UINTPTR)handle, bytes));
}

/* Optional user output range: NULL means the caller does not want a result. */
static RK_ERR kSyscallUserWriteOptional_(VOID *const ptr,
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

/*
 * Copy a packed syscall argument block after validating the whole input range.
 * Nested pointers are intentionally validated by the individual syscall case.
 */
static RK_ERR kSyscallUserStructCopy_(VOID const *const userPtr,
                                      VOID *const kernelPtr,
                                      ULONG const bytes)
{
    RK_ERR const err = kSyscallUserReadRequired_(userPtr, bytes);

    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    RK_MEMCPY(kernelPtr, userPtr, bytes);
    return (RK_ERR_SUCCESS);
}

static RK_ERR kSyscallUserFunctionRequired_(VOID const *const funPtr)
{
    if (funPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    return ((kMpuUserFunctionValid(funPtr) == RK_TRUE)
                ? RK_ERR_SUCCESS
                : RK_ERR_INVALID_PARAM);
}

static VOID kSyscallTaskStore_(RK_TCB *const taskPtr,
                               RK_EXCEPTION_FRAME *const framePtr,
                               ULONG const callNumber,
                               ULONG const arg0,
                               ULONG const arg1,
                               ULONG const arg2,
                               ULONG const arg3,
                               UINT const phase,
                               RK_ERR const wakeResult)
{
    if (taskPtr == NULL)
    {
        return;
    }

    taskPtr->syscallFramePtr = framePtr;
    taskPtr->syscallNumber = callNumber;
    taskPtr->syscallArg0 = arg0;
    taskPtr->syscallArg1 = arg1;
    taskPtr->syscallArg2 = arg2;
    taskPtr->syscallArg3 = arg3;
    taskPtr->syscallPhase = phase;
    taskPtr->syscallWakeResult = wakeResult;
}

static VOID kSyscallTaskSetReturn_(RK_TCB *const taskPtr, RK_ERR const ret)
{
    if ((taskPtr != NULL) && (taskPtr->syscallFramePtr != NULL))
    {
        taskPtr->syscallFramePtr->r0 = (ULONG)ret;
    }
}

static VOID kSyscallTaskRestart_(RK_TCB *const taskPtr)
{
    /* Tell kSyscallInvoke4() to issue SVC again using the saved TCB args. */
    kSyscallTaskSetReturn_(taskPtr, RK_ERR_SYSCALL_RESTART);
}

static VOID kSyscallTaskComplete_(RK_TCB *const taskPtr, RK_ERR const ret)
{
    /* Final result: write user-visible return value and forget saved args. */
    kSyscallTaskSetReturn_(taskPtr, ret);
    kSyscallTaskClear(taskPtr);
}

RK_BOOL kSyscallRequired(VOID)
{
    unsigned ipsrValue;
    ULONG controlValue;

    /* Handler mode is already privileged. Calling back through SVC from an ISR,
     * PendSV, SysTick, or SVC itself would be wrong and unnecessary. */
    RK_ASM volatile("MRS %0, IPSR" : "=r"(ipsrValue));
    if (ipsrValue != 0U)
    {
        return (RK_FALSE);
    }

    /* CONTROL.nPRIV == 1 means unprivileged thread mode. Those calls must enter
     * the kernel through SVC. */
    RK_ASM volatile("MRS %0, CONTROL" : "=r"(controlValue));
    return (((controlValue & 0x1UL) != 0UL) ? RK_TRUE : RK_FALSE);
}

VOID kSyscallTaskClear(RK_TCB *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return;
    }

    /* Clear all continuation state. This TCB no longer has a suspended syscall
     * that needs to be completed or restarted. */
    taskPtr->syscallFramePtr = NULL;
    taskPtr->syscallNumber = RK_SYSCALL_NONE;
    taskPtr->syscallArg0 = 0UL;
    taskPtr->syscallArg1 = 0UL;
    taskPtr->syscallArg2 = 0UL;
    taskPtr->syscallArg3 = 0UL;
    taskPtr->syscallPhase = RK_SYSCALL_PHASE_NONE;
    taskPtr->syscallWakeResult = RK_ERR_SUCCESS;
}

VOID kSyscallTaskSuspend(RK_EXCEPTION_FRAME *const framePtr,
                         ULONG const callNumber,
                         ULONG const arg0,
                         ULONG const arg1,
                         ULONG const arg2,
                         ULONG const arg3,
                         UINT const phase)
{
    if (RK_gRunPtr != NULL)
    {
        /* If a restarted syscall blocks again, keep the wake result already
         * recorded by the previous phase. This matters for multi-phase
         * operations such as condvar wait/relock. */
        RK_BOOL const samePending =
            ((RK_gRunPtr->syscallNumber == callNumber) &&
             (callNumber != RK_SYSCALL_NONE))
                ? RK_TRUE
                : RK_FALSE;
        RK_ERR const wakeResult = RK_gRunPtr->syscallWakeResult;

        kSyscallTaskStore_(
            RK_gRunPtr, framePtr, callNumber, arg0, arg1, arg2, arg3,
            phase, (samePending == RK_TRUE) ? wakeResult : RK_ERR_SUCCESS);
    }

    /* The user-mode wrapper loops while R0 is RK_ERR_SYSCALL_RESTART. */
    kSyscallSetFrameReturn_(framePtr, RK_ERR_SYSCALL_RESTART);
}

VOID kSyscallTaskCheckpoint(RK_EXCEPTION_FRAME *const framePtr,
                            ULONG const callNumber,
                            ULONG const arg0,
                            ULONG const arg1,
                            ULONG const arg2,
                            ULONG const arg3)
{
    if (RK_gRunPtr != NULL)
    {
        kSyscallTaskStore_(RK_gRunPtr, framePtr, callNumber, arg0, arg1,
                           arg2, arg3, RK_SYSCALL_PHASE_NONE,
                           RK_ERR_SUCCESS);
    }

    kSyscallSetFrameReturn_(framePtr, RK_ERR_SYSCALL_RESTART);
}

#if (RK_CONF_SVC_DEFER_TEST == ON)
static RK_ERR kSyscallTestSysTickDefer_(RK_EXCEPTION_FRAME *const framePtr,
                                        ULONG const iterations)
{
    ULONG remaining = iterations;
    ULONG acc = RK_gSyscallTestSpinSink;

    while (remaining != 0UL)
    {
        ULONG const chunk =
            (remaining > RK_SYSCALL_TEST_SPIN_CHUNK) ?
                RK_SYSCALL_TEST_SPIN_CHUNK :
                remaining;

        for (ULONG i = 0UL; i < chunk; i++)
        {
            acc += ((remaining - i) ^ (acc << 1U)) + 1UL;
            RK_BARRIER
        }

        remaining -= chunk;
        if ((remaining != 0UL) &&
            (kSyscallPreemptPending() == RK_TRUE))
        {
            RK_gSyscallTestSpinSink = acc;
            kSyscallTaskCheckpoint(framePtr, RK_SYSCALL_TEST_SYSTICK_DEFER,
                                   remaining, 0UL, 0UL, 0UL);
            return (RK_ERR_SYSCALL_RESTART);
        }
    }

    RK_gSyscallTestSpinSink = acc;
    if ((RK_gRunPtr != NULL) &&
        (RK_gRunPtr->syscallNumber == RK_SYSCALL_TEST_SYSTICK_DEFER))
    {
        kSyscallTaskClear(RK_gRunPtr);
    }
    return (RK_ERR_SUCCESS);
}
#endif

VOID kSyscallTaskWake(RK_TCB *const taskPtr)
{
    if ((taskPtr == NULL) || (taskPtr->syscallNumber == RK_SYSCALL_NONE))
    {
        return;
    }

    switch (taskPtr->syscallNumber)
    {
        /* These wait paths do not need to copy deferred data into a user buffer
         * after wake. The wake itself is the successful completion. */
        case RK_SYSCALL_SLEEP_DELAY:
        case RK_SYSCALL_SLEEP_RELEASE:
        case RK_SYSCALL_SLEEP_UNTIL:
        case RK_SYSCALL_SEMAPHORE_PEND:
        case RK_SYSCALL_SLEEP_QUEUE_SLEEP:
        case RK_SYSCALL_MESG_ALLOC:
            kSyscallTaskComplete_(taskPtr, RK_ERR_SUCCESS);
            break;

        case RK_SYSCALL_MUTEX_LOCK:
            kSyscallTaskComplete_(taskPtr, taskPtr->syscallWakeResult);
            break;

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
        case RK_SYSCALL_MESG_WAIT:
        {
            RK_ERR const ret = taskPtr->asynchMesgWaitStatus;
            taskPtr->asynchMesgWaitSenderPtr = NULL;
            taskPtr->asynchMesgWaitDestPtr = NULL;
            taskPtr->asynchMesgWaitStatus = RK_ERR_SUCCESS;
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, ret);
            break;
        }
#if (RK_CONF_ASYNCH_COPY_MESG == ON)
        case RK_SYSCALL_MESG_RECV_COPY:
        {
            RK_ERR const ret = taskPtr->asynchCopyMesgRecvStatus;
            taskPtr->asynchCopyMesgWaitSenderPtr = NULL;
            taskPtr->asynchCopyMesgRecvBufPtr = NULL;
            taskPtr->asynchCopyMesgRecvBufBytes = 0UL;
            taskPtr->asynchCopyMesgRecvBytesPtr = NULL;
            taskPtr->asynchCopyMesgRecvStatus = RK_ERR_SUCCESS;
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, ret);
            break;
        }
#endif
#endif

        case RK_SYSCALL_CONDVAR_WAIT:
            /* Condvar wait is two-phase: wake from the condition, then relock
             * the mutex before returning to the caller. */
            if (taskPtr->syscallPhase == RK_SYSCALL_PHASE_CONDVAR_RELOCK)
            {
                kSyscallTaskComplete_(taskPtr, taskPtr->syscallWakeResult);
            }
            else
            {
                taskPtr->syscallWakeResult = RK_ERR_SUCCESS;
                taskPtr->syscallPhase = RK_SYSCALL_PHASE_CONDVAR_RELOCK;
                kSyscallTaskRestart_(taskPtr);
            }
            break;

        /* These services must run again after wake to re-check object state or
         * copy data while privileged. */
        case RK_SYSCALL_EVENT_GET:
        case RK_SYSCALL_MESG_QUEUE_SEND:
        case RK_SYSCALL_MESG_QUEUE_JAM:
        case RK_SYSCALL_MESG_QUEUE_BROADCAST_RECV:
            kSyscallTaskRestart_(taskPtr);
            break;

#if (RK_CONF_MESG_QUEUE == ON)
        case RK_SYSCALL_MESG_QUEUE_RECV:
            /* Direct-delivery receive has already copied the message into the
             * receiver's user buffer in privileged context. Normal queue wake
             * needs a restart so the receive service can copy from the queue. */
            if (taskPtr->timeoutNode.waitInfo == RK_MESGQ_RECV_DIRECT_DELIVER)
            {
                taskPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
                kSyscallTaskComplete_(taskPtr, RK_ERR_SUCCESS);
            }
            else
            {
                kSyscallTaskRestart_(taskPtr);
            }
            break;
#endif

#if (RK_CONF_SYNCH_MESG == ON)
        case RK_SYSCALL_SYNCH_SEND_WAIT:
        {
            RK_ERR const ret = taskPtr->synchMesgStatus;
            taskPtr->synchMesgStatus = RK_ERR_SUCCESS;
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, ret);
            break;
        }

        case RK_SYSCALL_SYNCH_RECV:
        {
            RK_ERR const ret = taskPtr->synchMesgRecvStatus;
            taskPtr->synchMesgRecvStatus = RK_ERR_SUCCESS;
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, ret);
            break;
        }

        case RK_SYSCALL_SYNCH_MESG_CALL:
        {
            RK_ERR const ret = taskPtr->synchMesgStatus;
            taskPtr->synchMesgStatus = RK_ERR_SUCCESS;
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, ret);
            break;
        }

        case RK_SYSCALL_SYNCH_MESG_ACCEPT:
            kSyscallTaskRestart_(taskPtr);
            break;
#endif

        default:
            /* Non-blocking or simple blocking services complete successfully
             * unless a specialised case above says otherwise. */
            kSyscallTaskComplete_(taskPtr, RK_ERR_SUCCESS);
            break;
    }
}

VOID kSyscallTaskTimeout(RK_TCB *const taskPtr)
{
    if ((taskPtr == NULL) || (taskPtr->syscallNumber == RK_SYSCALL_NONE))
    {
        return;
    }

    switch (taskPtr->syscallNumber)
    {
        /* Sleep timeout means the requested delay elapsed successfully. */
        case RK_SYSCALL_SLEEP_DELAY:
        case RK_SYSCALL_SLEEP_RELEASE:
        case RK_SYSCALL_SLEEP_UNTIL:
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_SUCCESS);
            break;

        case RK_SYSCALL_CONDVAR_WAIT:
            /* Timeout still has to relock the mutex before the user sees
             * RK_ERR_TIMEOUT. */
            taskPtr->timeOut = RK_FALSE;
            taskPtr->syscallWakeResult = RK_ERR_TIMEOUT;
            taskPtr->syscallPhase = RK_SYSCALL_PHASE_CONDVAR_RELOCK;
            kSyscallTaskRestart_(taskPtr);
            break;

        case RK_SYSCALL_MUTEX_LOCK:
#if (RK_CONF_MUTEX == ON)
            /* The task is no longer waiting on this mutex after timeout. */
            taskPtr->waitingForMutexPtr = NULL;
#endif
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_TIMEOUT);
            break;

        case RK_SYSCALL_MESG_QUEUE_RECV:
        case RK_SYSCALL_MESG_QUEUE_BROADCAST_RECV:
#if (RK_CONF_MESG_QUEUE == ON)
            /* Do not leave stale receive-buffer or broadcast-wait state in the
             * TCB once the wait has timed out. */
            taskPtr->mesgQueueRecvBufPtr = NULL;
            taskPtr->timeoutNode.waitInfo = RK_MESGQ_RECV_WAIT_NORMAL;
#endif
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_TIMEOUT);
            break;

        case RK_SYSCALL_MESG_ALLOC:
#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
            taskPtr->asynchMesgAllocDestPtr = NULL;
#endif
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_TIMEOUT);
            break;

        case RK_SYSCALL_MESG_WAIT:
#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
            taskPtr->asynchMesgWaitSenderPtr = NULL;
            taskPtr->asynchMesgWaitDestPtr = NULL;
            taskPtr->asynchMesgWaitStatus = RK_ERR_SUCCESS;
#endif
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_TIMEOUT);
            break;

        case RK_SYSCALL_MESG_RECV_COPY:
#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON) &&            \
     (RK_CONF_ASYNCH_COPY_MESG == ON))
            taskPtr->asynchCopyMesgWaitSenderPtr = NULL;
            taskPtr->asynchCopyMesgRecvBufPtr = NULL;
            taskPtr->asynchCopyMesgRecvBufBytes = 0UL;
            taskPtr->asynchCopyMesgRecvBytesPtr = NULL;
            taskPtr->asynchCopyMesgRecvStatus = RK_ERR_SUCCESS;
#endif
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_TIMEOUT);
            break;

#if (RK_CONF_SYNCH_MESG == ON)
        case RK_SYSCALL_SYNCH_SEND_WAIT:
            taskPtr->synchMesgStatus = RK_ERR_SUCCESS;
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_TIMEOUT);
            break;

        case RK_SYSCALL_SYNCH_RECV:
            taskPtr->synchMesgRecvBufPtr = NULL;
            taskPtr->synchMesgRecvBytesPtr = NULL;
            taskPtr->synchMesgRecvStatus = RK_ERR_SUCCESS;
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_TIMEOUT);
            break;

        case RK_SYSCALL_SYNCH_MESG_CALL:
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_TIMEOUT);
            break;

        case RK_SYSCALL_SYNCH_MESG_ACCEPT:
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_TIMEOUT);
            break;
#endif

        default:
            taskPtr->timeOut = RK_FALSE;
            kSyscallTaskComplete_(taskPtr, RK_ERR_TIMEOUT);
            break;
    }
}

static RK_BOOL kSyscallThreadOrigin_(ULONG const excReturn)
{
    /* EXC_RETURN bit 2 is set when the exception came from thread mode using
     * PSP. Kernel-origin SVC calls are rejected by kSyscallDispatch(). */
    return (((excReturn & 0x4UL) != 0UL) ? RK_TRUE : RK_FALSE);
}

static VOID kSyscallDispatchActive_(RK_EXCEPTION_FRAME *const framePtr)
{
    /* kSyscall4_ places the syscall number and first four arguments in the
     * stacked core registers. R12 is used for arg3. */
    ULONG callNumber = framePtr->r0;
    ULONG arg0 = framePtr->r1;
    ULONG arg1 = framePtr->r2;
    ULONG arg2 = framePtr->r3;
    ULONG arg3 = framePtr->r12;
    RK_TCB *const taskPtr = RK_gRunPtr;
    RK_ERR ret = RK_ERR_INVALID_PARAM;

    if ((taskPtr != NULL) && (taskPtr->syscallNumber != RK_SYSCALL_NONE))
    {
        /* A task that blocked in a syscall may only restart that same syscall.
         * Its original arguments come from the TCB, not from user registers. */
        if (callNumber != taskPtr->syscallNumber)
        {
            kSyscallTaskClear(taskPtr);
            kSyscallSetFrameReturn_(framePtr, RK_ERR_INVALID_PARAM);
            return;
        }

        taskPtr->syscallFramePtr = framePtr;
        arg0 = taskPtr->syscallArg0;
        arg1 = taskPtr->syscallArg1;
        arg2 = taskPtr->syscallArg2;
        arg3 = taskPtr->syscallArg3;
    }

    switch (callNumber)
    {
        /* Scheduler and task-query services. */
        case RK_SYSCALL_YIELD:
            kYield();
            ret = RK_ERR_SUCCESS;
            break;

        case RK_SYSCALL_SCH_LOCK:
            kSchLock();
            ret = RK_ERR_SUCCESS;
            break;

        case RK_SYSCALL_SCH_UNLOCK:
            kSchUnlock();
            ret = RK_ERR_SUCCESS;
            break;

        case RK_SYSCALL_TASK_GET_RUNNING_HANDLE:
            framePtr->r0 = (ULONG)(UINTPTR)kTaskGetRunningHandle();
            return;

        case RK_SYSCALL_TASK_GET_ID:
            framePtr->r0 = (ULONG)kTaskGetID((RK_TASK_HANDLE)(UINTPTR)arg0);
            return;

        case RK_SYSCALL_TASK_GET_RUNNING_NAME:
            framePtr->r0 = (ULONG)(UINTPTR)kTaskGetRunningName();
            return;

        case RK_SYSCALL_TASK_GET_NAME:
            ret = kSyscallUserWriteRequired_((CHAR *)(UINTPTR)arg1,
                                             RK_OBJ_MAX_NAME_LEN);
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kTaskGetName((RK_TASK_HANDLE)(UINTPTR)arg0,
                                   (CHAR *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_TASK_GET_PRIO:
            framePtr->r0 = (ULONG)kTaskGetPrio((RK_TASK_HANDLE)(UINTPTR)arg0);
            return;

        case RK_SYSCALL_TASK_GET_NOM_PRIO:
            framePtr->r0 =
                (ULONG)kTaskGetNomPrio((RK_TASK_HANDLE)(UINTPTR)arg0);
            return;

        case RK_SYSCALL_TASK_INIT:
        {
            RK_TASK_INIT_SYSCALL_ARGS args;
            ULONG stackBytes = 0UL;

            ret = kSyscallUserStructCopy_((VOID const *)(UINTPTR)arg0,
                                          &args, sizeof(args));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.taskHandlePtr,
                                                 sizeof(RK_TASK_HANDLE));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserFunctionRequired_(
                    (VOID const *)(UINTPTR)args.taskFunc);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserReadRequired_(args.taskName,
                                                RK_OBJ_MAX_NAME_LEN);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(args.stackSize,
                                       (ULONG)sizeof(RK_STACK),
                                       &stackBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.stackBufPtr,
                                                 stackBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kTaskInit(args.taskHandlePtr, args.taskFunc,
                                args.argsPtr, args.taskName,
                                args.stackBufPtr, args.stackSize,
                                args.priority, args.preempt);
            }
            break;
        }

        case RK_SYSCALL_TASK_INIT_MODULE:
        {
            RK_TASK_INIT_SYSCALL_ARGS args;
            ULONG stackBytes = 0UL;

            ret = kSyscallUserStructCopy_((VOID const *)(UINTPTR)arg0,
                                          &args, sizeof(args));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.taskHandlePtr,
                                                 sizeof(RK_TASK_HANDLE));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserFunctionRequired_(
                    (VOID const *)(UINTPTR)args.taskFunc);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserReadRequired_(args.taskName,
                                                RK_OBJ_MAX_NAME_LEN);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserReadRequired_(args.modulePtr,
                                                sizeof(RK_MODULE));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(args.stackSize,
                                       (ULONG)sizeof(RK_STACK),
                                       &stackBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.stackBufPtr,
                                                 stackBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kTaskInitModule(args.taskHandlePtr,
                                      args.taskFunc, args.argsPtr,
                                      args.taskName, args.stackBufPtr,
                                      args.stackSize, args.priority,
                                      args.preempt, args.modulePtr);
            }
            break;
        }

        case RK_SYSCALL_TASK_INIT_PROTECTED:
        {
            RK_TASK_PROTECTED_INIT_SYSCALL_ARGS args;
            RK_TASK_MEMORY memory;
            ULONG stackBytes = 0UL;

            ret = kSyscallUserStructCopy_((VOID const *)(UINTPTR)arg0,
                                          &args, sizeof(args));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserFunctionRequired_(
                    (VOID const *)(UINTPTR)args.taskFunc);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserStructCopy_(args.memoryPtr, &memory,
                                              sizeof(memory));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(memory.stackWords,
                                       (ULONG)sizeof(RK_STACK),
                                       &stackBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(memory.stackBasePtr,
                                                 stackBytes);
            }
            if ((ret == RK_ERR_SUCCESS) && (memory.modulePtr != NULL))
            {
                ret = kSyscallUserReadRequired_(memory.modulePtr,
                                                sizeof(RK_MODULE));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kTaskInitProtected(args.taskPtr, args.taskFunc,
                                         args.argsPtr, args.priority,
                                         &memory);
            }
            break;
        }

#if (RK_CONF_DYNAMIC_TASK == ON)
        case RK_SYSCALL_TASK_SPAWN:
        {
            RK_DYNAMIC_TASK_ATTR attr;

            ret = kSyscallUserStructCopy_((VOID const *)(UINTPTR)arg0,
                                          &attr, sizeof(attr));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(
                    (RK_TASK_HANDLE *)(UINTPTR)arg1,
                    sizeof(RK_TASK_HANDLE));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserFunctionRequired_(
                    (VOID const *)(UINTPTR)attr.taskFunc);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserReadRequired_(attr.taskName,
                                                RK_OBJ_MAX_NAME_LEN);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserReadRequired_(attr.stackMemPtr,
                                                sizeof(RK_MEM_PARTITION));
            }
            if ((ret == RK_ERR_SUCCESS) && (attr.modulePtr != NULL))
            {
                ret = kSyscallUserReadRequired_(attr.modulePtr,
                                                sizeof(RK_MODULE));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kTaskSpawn(&attr, (RK_TASK_HANDLE *)(UINTPTR)arg1);
            }
            break;
        }

        case RK_SYSCALL_TASK_TERMINATE:
            ret = kSyscallUserWriteRequired_((RK_TASK_HANDLE *)(UINTPTR)arg0,
                                             sizeof(RK_TASK_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kTaskTerminate((RK_TASK_HANDLE *)(UINTPTR)arg0);
            }
            break;

        case RK_SYSCALL_TASK_TERMINATE_SELF:
            ret = kTaskTerminateSelf();
            break;
#endif

        case RK_SYSCALL_MODULE_INIT:
            ret = kSyscallUserWriteRequired_((RK_MODULE *)(UINTPTR)arg0,
                                             sizeof(RK_MODULE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_((BYTE *)(UINTPTR)arg1,
                                                 arg2);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserReadRequired_((CHAR *)(UINTPTR)arg3,
                                                RK_OBJ_MAX_NAME_LEN);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kModuleInit((RK_MODULE *)(UINTPTR)arg0,
                                  (BYTE *)(UINTPTR)arg1, arg2,
                                  (CHAR *)(UINTPTR)arg3);
            }
            break;

        case RK_SYSCALL_SHARED_REGION_INIT:
            ret = kSyscallUserWriteRequired_(
                (RK_SHARED_REGION *)(UINTPTR)arg0, sizeof(RK_SHARED_REGION));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_((BYTE *)(UINTPTR)arg1,
                                                 arg2);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSharedRegionInit((RK_SHARED_REGION *)(UINTPTR)arg0,
                                        (BYTE *)(UINTPTR)arg1, arg2);
            }
            break;

        case RK_SYSCALL_MODULE_MAP_SHARED_REGION:
            ret = kSyscallUserWriteRequired_((RK_MODULE *)(UINTPTR)arg0,
                                             sizeof(RK_MODULE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserReadRequired_(
                    (RK_SHARED_REGION *)(UINTPTR)arg1,
                    sizeof(RK_SHARED_REGION));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kModuleMapSharedRegion((RK_MODULE *)(UINTPTR)arg0,
                                             (RK_SHARED_REGION *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_SHARED_MEM_CREATE:
            ret = kSyscallUserWriteRequired_(
                (RK_SHARED_MEM_HANDLE *)(UINTPTR)arg0,
                sizeof(RK_SHARED_MEM_HANDLE));
            if ((ret == RK_ERR_SUCCESS) &&
                (kMpuLayoutIsFinalized() == RK_TRUE))
            {
                ret = RK_ERR_INVALID_PHASE;
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_((VOID *)(UINTPTR)arg1,
                                                 arg2);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSharedMemCreate(
                    (RK_SHARED_MEM_HANDLE *)(UINTPTR)arg0,
                    (VOID *)(UINTPTR)arg1, arg2);
            }
            break;

        case RK_SYSCALL_SHARED_MEM_DESTROY:
            ret = kSyscallUserWriteRequired_(
                (RK_SHARED_MEM_HANDLE *)(UINTPTR)arg0,
                sizeof(RK_SHARED_MEM_HANDLE));
            if ((ret == RK_ERR_SUCCESS) &&
                (kMpuLayoutIsFinalized() == RK_TRUE))
            {
                ret = RK_ERR_INVALID_PHASE;
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSharedMemDestroy(
                    (RK_SHARED_MEM_HANDLE *)(UINTPTR)arg0);
            }
            break;

        case RK_SYSCALL_SHARED_MEM_ATTACH:
            ret = kSyscallRawHandleReadRequired_(
                (RK_HANDLE)arg0, sizeof(RK_SHARED_MEM));
            if ((ret == RK_ERR_SUCCESS) &&
                (kMpuLayoutIsFinalized() == RK_TRUE))
            {
                ret = RK_ERR_INVALID_PHASE;
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(
                    (RK_MODULE *)(UINTPTR)arg1, sizeof(RK_MODULE));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSharedMemAttach((RK_SHARED_MEM_HANDLE)arg0,
                                       (RK_MODULE *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_SHARED_MEM_DETACH:
            ret = kSyscallRawHandleReadRequired_(
                (RK_HANDLE)arg0, sizeof(RK_SHARED_MEM));
            if ((ret == RK_ERR_SUCCESS) &&
                (kMpuLayoutIsFinalized() == RK_TRUE))
            {
                ret = RK_ERR_INVALID_PHASE;
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(
                    (RK_MODULE *)(UINTPTR)arg1, sizeof(RK_MODULE));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSharedMemDetach((RK_SHARED_MEM_HANDLE)arg0,
                                       (RK_MODULE *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_SHARED_MEM_GET:
            ret = kSyscallRawHandleReadRequired_(
                (RK_HANDLE)arg0, sizeof(RK_SHARED_MEM));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(
                    (VOID **)(UINTPTR)arg1, sizeof(VOID *));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(
                    (ULONG *)(UINTPTR)arg2, sizeof(ULONG));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSharedMemGet((RK_SHARED_MEM_HANDLE)arg0,
                                    (VOID **)(UINTPTR)arg1,
                                    (ULONG *)(UINTPTR)arg2);
            }
            break;

        case RK_SYSCALL_OBJ_PARTITIONS_INIT:
            ret = kObjPartitionsInit();
            break;

        /* Time and blocking-delay services. */
        case RK_SYSCALL_TICK_GET:
            framePtr->r0 = (ULONG)kTickGet();
            return;

        case RK_SYSCALL_TICK_GET_MS:
            framePtr->r0 = (ULONG)kTickGetMs();
            return;

        case RK_SYSCALL_SLEEP_DELAY:
            ret = kSleepDelaySyscall(framePtr, (RK_TICK)arg0);
            break;

        case RK_SYSCALL_SLEEP_RELEASE:
            ret = kSleepReleaseSyscall(framePtr, (RK_TICK)arg0);
            break;

        case RK_SYSCALL_SLEEP_UNTIL:
            ret = kSyscallUserWriteRequired_((RK_TICK *)(UINTPTR)arg0,
                                             sizeof(RK_TICK));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSleepUntilSyscall(framePtr, (RK_TICK *)(UINTPTR)arg0,
                                         (RK_TICK)arg1);
            }
            break;

        case RK_SYSCALL_DELAY:
            ret = kDelay((RK_TICK)arg0);
            break;

        /* Fixed-block allocator services. */
        case RK_SYSCALL_MEM_PARTITION_ALLOC:
        {
            RK_MEM_PARTITION *const partPtr =
                (RK_MEM_PARTITION *)(UINTPTR)arg0;
            VOID *allocPtr;

            if (partPtr == NULL)
            {
                framePtr->r0 = 0UL;
                return;
            }

            allocPtr = kMemPartitionAlloc(partPtr);

            if ((allocPtr != NULL) &&
                ((partPtr == NULL) ||
                 (kMpuUserWriteValid(RK_gRunPtr, allocPtr,
                                     partPtr->blkSize) != RK_TRUE)))
            {
                (VOID)kMemPartitionFree(partPtr, allocPtr);
                allocPtr = NULL;
            }

            framePtr->r0 = (ULONG)(UINTPTR)allocPtr;
            return;
        }

        case RK_SYSCALL_MEM_PARTITION_FREE:
            ret = kSyscallUserWriteRequired_((VOID *)(UINTPTR)arg1,
                                             sizeof(VOID *));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMemPartitionFree((RK_MEM_PARTITION *)(UINTPTR)arg0,
                                        (VOID *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_MEM_PARTITION_INIT:
        {
            ULONG poolBytes = 0UL;

            ret = kSyscallUserWriteRequired_(
                (RK_MEM_PARTITION *)(UINTPTR)arg0,
                sizeof(RK_MEM_PARTITION));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(arg2, arg3, &poolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_((VOID *)(UINTPTR)arg1,
                                                 poolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMemPartitionInit((RK_MEM_PARTITION *)(UINTPTR)arg0,
                                        (VOID *)(UINTPTR)arg1, arg2, arg3);
            }
            break;
        }

        /* Per-task event flags. */
        case RK_SYSCALL_EVENT_GET:
            ret = kSyscallUserWriteOptional_(
                (RK_TASK_EVENT *)(UINTPTR)arg2, sizeof(RK_TASK_EVENT));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kEventGetSyscall(framePtr, (RK_TASK_EVENT)arg0,
                                       (RK_OPTION)arg1,
                                       (RK_TASK_EVENT *)(UINTPTR)arg2,
                                       (RK_TICK)arg3);
            }
            break;

        case RK_SYSCALL_EVENT_SET:
            ret = kEventSet((RK_TASK_HANDLE)(UINTPTR)arg0,
                            (RK_TASK_EVENT)arg1);
            break;

        case RK_SYSCALL_EVENT_CLEAR:
            ret = kEventClear((RK_TASK_HANDLE)(UINTPTR)arg0,
                              (RK_TASK_EVENT)arg1);
            break;

        case RK_SYSCALL_EVENT_QUERY:
            ret = kSyscallUserWriteRequired_(
                (RK_TASK_EVENT *)(UINTPTR)arg1, sizeof(RK_TASK_EVENT));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kEventQuery((RK_TASK_HANDLE)(UINTPTR)arg0,
                                  (RK_TASK_EVENT *)(UINTPTR)arg1);
            }
            break;

#if (RK_CONF_SEMAPHORE == ON)
        /* Semaphores. */
        case RK_SYSCALL_SEMAPHORE_INIT:
            ret = kSyscallUserWriteRequired_(
                (RK_SEMAPHORE *)(UINTPTR)arg0, sizeof(RK_SEMAPHORE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSemaphoreInit((RK_SEMAPHORE *)(UINTPTR)arg0,
                                     (UINT)arg1, (UINT)arg2);
            }
            break;

        case RK_SYSCALL_SEMAPHORE_PEND:
            ret = kSyscallRawHandleWriteRequired_((RK_HANDLE)arg0,
                                                  sizeof(RK_SEMAPHORE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSemaphorePendSyscall(framePtr,
                                            (RK_SEMAPHORE_HANDLE)(UINTPTR)arg0,
                                            (RK_TICK)arg1);
            }
            break;

        case RK_SYSCALL_SEMAPHORE_POST:
            ret = kSyscallRawHandleWriteRequired_((RK_HANDLE)arg0,
                                                  sizeof(RK_SEMAPHORE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSemaphorePost((RK_SEMAPHORE_HANDLE)(UINTPTR)arg0);
            }
            break;

        case RK_SYSCALL_SEMAPHORE_QUERY:
            ret = kSyscallRawHandleReadRequired_((RK_HANDLE)arg0,
                                                 sizeof(RK_SEMAPHORE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_((INT *)(UINTPTR)arg1,
                                                 sizeof(INT));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSemaphoreQuery((RK_SEMAPHORE_HANDLE)(UINTPTR)arg0,
                                      (INT *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_SEMAPHORE_CREATE:
            ret = kSyscallUserWriteRequired_(
                (RK_SEMAPHORE_HANDLE *)(UINTPTR)arg0,
                sizeof(RK_SEMAPHORE_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSemaphoreCreate((RK_SEMAPHORE_HANDLE *)(UINTPTR)arg0,
                                       (UINT)arg1, (UINT)arg2);
            }
            break;

        case RK_SYSCALL_SEMAPHORE_DESTROY:
            ret = kSyscallUserWriteRequired_(
                (RK_SEMAPHORE_HANDLE *)(UINTPTR)arg0,
                sizeof(RK_SEMAPHORE_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSemaphoreDestroy((RK_SEMAPHORE_HANDLE *)(UINTPTR)arg0);
            }
            break;
#endif

#if (RK_CONF_MUTEX == ON)
        /* Mutexes. */
        case RK_SYSCALL_MUTEX_INIT:
            ret = kSyscallUserWriteRequired_((RK_MUTEX *)(UINTPTR)arg0,
                                             sizeof(RK_MUTEX));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMutexInit((RK_MUTEX *)(UINTPTR)arg0, (UINT)arg1);
            }
            break;

        case RK_SYSCALL_MUTEX_LOCK:
            ret = kMutexLockSyscall(framePtr, (RK_MUTEX_HANDLE)(UINTPTR)arg0,
                                    (RK_TICK)arg1);
            break;

        case RK_SYSCALL_MUTEX_UNLOCK:
            ret = kMutexUnlock((RK_MUTEX_HANDLE)(UINTPTR)arg0);
            break;

        case RK_SYSCALL_MUTEX_QUERY:
            ret = kSyscallUserWriteRequired_((UINT *)(UINTPTR)arg1,
                                             sizeof(UINT));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMutexQuery((RK_MUTEX_HANDLE)(UINTPTR)arg0,
                                  (UINT *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_MUTEX_CREATE:
            ret = kSyscallUserWriteRequired_(
                (RK_MUTEX_HANDLE *)(UINTPTR)arg0, sizeof(RK_MUTEX_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMutexCreate((RK_MUTEX_HANDLE *)(UINTPTR)arg0,
                                   (UINT)arg1);
            }
            break;

        case RK_SYSCALL_MUTEX_DESTROY:
            ret = kSyscallUserWriteRequired_(
                (RK_MUTEX_HANDLE *)(UINTPTR)arg0, sizeof(RK_MUTEX_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMutexDestroy((RK_MUTEX_HANDLE *)(UINTPTR)arg0);
            }
            break;
#endif

#if (RK_CONF_SLEEP_QUEUE == ON)
        /* Sleep queues and condition variables. */
        case RK_SYSCALL_SLEEP_QUEUE_INIT:
            ret = kSyscallUserWriteRequired_(
                (RK_SLEEP_QUEUE *)(UINTPTR)arg0, sizeof(RK_SLEEP_QUEUE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSleepQueueInit((RK_SLEEP_QUEUE *)(UINTPTR)arg0);
            }
            break;

        case RK_SYSCALL_SLEEP_QUEUE_SLEEP:
            ret = kSleepQueueSleepSyscall(framePtr,
                                          (RK_SLEEP_QUEUE_HANDLE)(UINTPTR)arg0,
                                          (RK_TICK)arg1);
            break;

        case RK_SYSCALL_SLEEP_QUEUE_SIGNAL:
            ret = kSleepQueueSignal((RK_SLEEP_QUEUE_HANDLE)(UINTPTR)arg0);
            break;

        case RK_SYSCALL_SLEEP_QUEUE_READY:
            ret = kSleepQueueReady((RK_SLEEP_QUEUE_HANDLE)(UINTPTR)arg0,
                                   (RK_TASK_HANDLE)(UINTPTR)arg1);
            break;

        case RK_SYSCALL_SLEEP_QUEUE_UNREADY:
            ret = kSleepQueueUnready((RK_SLEEP_QUEUE_HANDLE)(UINTPTR)arg0,
                                     (RK_TASK_HANDLE)(UINTPTR)arg1);
            break;

        case RK_SYSCALL_SLEEP_QUEUE_QUERY:
            ret = kSyscallUserWriteRequired_((ULONG *)(UINTPTR)arg1,
                                             sizeof(ULONG));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSleepQueueQuery((RK_SLEEP_QUEUE_HANDLE)(UINTPTR)arg0,
                                       (ULONG *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_SLEEP_QUEUE_WAKE:
            ret = kSyscallUserWriteOptional_((UINT *)(UINTPTR)arg2,
                                             sizeof(UINT));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSleepQueueWake((RK_SLEEP_QUEUE_HANDLE)(UINTPTR)arg0,
                                      (UINT)arg1, (UINT *)(UINTPTR)arg2);
            }
            break;

        case RK_SYSCALL_SLEEP_QUEUE_CREATE:
            ret = kSyscallUserWriteRequired_(
                (RK_SLEEP_QUEUE_HANDLE *)(UINTPTR)arg0,
                sizeof(RK_SLEEP_QUEUE_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSleepQueueCreate((RK_SLEEP_QUEUE_HANDLE *)(UINTPTR)arg0);
            }
            break;

        case RK_SYSCALL_SLEEP_QUEUE_DESTROY:
            ret = kSyscallUserWriteRequired_(
                (RK_SLEEP_QUEUE_HANDLE *)(UINTPTR)arg0,
                sizeof(RK_SLEEP_QUEUE_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSleepQueueDestroy((RK_SLEEP_QUEUE_HANDLE *)(UINTPTR)arg0);
            }
            break;

#if (RK_CONF_CONDVAR == ON)
        case RK_SYSCALL_CONDVAR_INIT:
            ret = kSyscallUserWriteRequired_(
                (RK_SLEEP_QUEUE *)(UINTPTR)arg0, sizeof(RK_SLEEP_QUEUE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kCondVarInit((RK_SLEEP_QUEUE *)(UINTPTR)arg0,
                                   (RK_MUTEX *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_CONDVAR_WAIT:
            ret = kCondVarWaitSyscall(framePtr,
                                      (RK_SLEEP_QUEUE_HANDLE)(UINTPTR)arg0,
                                      (RK_MUTEX_HANDLE)(UINTPTR)arg1,
                                      (RK_TICK)arg2);
            break;

        case RK_SYSCALL_CONDVAR_SIGNAL:
            ret = kCondVarSignal((RK_SLEEP_QUEUE_HANDLE)(UINTPTR)arg0);
            break;

        case RK_SYSCALL_CONDVAR_BROADCAST:
            ret = kCondVarBroadcast((RK_SLEEP_QUEUE_HANDLE)(UINTPTR)arg0);
            break;
#endif
#endif

#if (RK_CONF_MESG_QUEUE == ON)
        /* Message queues and mailbox aliases. */
        case RK_SYSCALL_MESG_QUEUE_INIT:
        {
            ULONG bufWords = 0UL;
            ULONG bufBytes = 0UL;

            ret = kSyscallUserWriteRequired_(
                (RK_MESG_QUEUE *)(UINTPTR)arg0, sizeof(RK_MESG_QUEUE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(arg2, arg3, &bufWords);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(bufWords, RK_WORD_SIZE, &bufBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_((VOID *)(UINTPTR)arg1,
                                                 bufBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgQueueInit((RK_MESG_QUEUE *)(UINTPTR)arg0,
                                     (VOID *)(UINTPTR)arg1, arg2, arg3);
            }
            break;
        }

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)
        case RK_SYSCALL_MESG_QUEUE_INSTALL_SEND_CBK:
            ret = kSyscallUserFunctionRequired_((VOID const *)(UINTPTR)arg1);
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgQueueInstallSendCbk(
                    (RK_MESG_QUEUE_HANDLE)(UINTPTR)arg0,
                    (VOID (*)(RK_MESG_QUEUE *))(UINTPTR)arg1);
            }
            break;
#endif

        case RK_SYSCALL_MESG_QUEUE_SEND:
            ret = kMesgQueueSendSyscall(framePtr,
                                        (RK_MESG_QUEUE_HANDLE)(UINTPTR)arg0,
                                        (VOID *)(UINTPTR)arg1,
                                        (RK_TICK)arg2);
            break;

        case RK_SYSCALL_MESG_QUEUE_RECV:
            ret = kMesgQueueRecvSyscall(framePtr,
                                        (RK_MESG_QUEUE_HANDLE)(UINTPTR)arg0,
                                        (VOID *)(UINTPTR)arg1,
                                        (RK_TICK)arg2);
            break;

        case RK_SYSCALL_MESG_QUEUE_PEEK:
            ret = kMesgQueuePeek((RK_MESG_QUEUE_HANDLE)(UINTPTR)arg0,
                                 (VOID *)(UINTPTR)arg1);
            break;

        case RK_SYSCALL_MESG_QUEUE_JAM:
            ret = kMesgQueueJamSyscall(framePtr,
                                       (RK_MESG_QUEUE_HANDLE)(UINTPTR)arg0,
                                       (VOID *)(UINTPTR)arg1,
                                       (RK_TICK)arg2);
            break;

        case RK_SYSCALL_MESG_QUEUE_QUERY:
            ret = kSyscallUserWriteOptional_((UINT *)(UINTPTR)arg1,
                                             sizeof(UINT));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteOptional_((UINT *)(UINTPTR)arg2,
                                                 sizeof(UINT));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteOptional_((UINT *)(UINTPTR)arg3,
                                                 sizeof(UINT));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgQueueQuery((RK_MESG_QUEUE_HANDLE)(UINTPTR)arg0,
                                      (UINT *)(UINTPTR)arg1,
                                      (UINT *)(UINTPTR)arg2,
                                      (UINT *)(UINTPTR)arg3);
            }
            break;

        case RK_SYSCALL_MESG_QUEUE_RESET:
            ret = kMesgQueueReset((RK_MESG_QUEUE_HANDLE)(UINTPTR)arg0);
            break;

        case RK_SYSCALL_MESG_QUEUE_POST_OVW:
            ret = kMesgQueuePostOvw((RK_MESG_QUEUE_HANDLE)(UINTPTR)arg0,
                                    (VOID *)(UINTPTR)arg1);
            break;

        case RK_SYSCALL_MESG_QUEUE_BROADCAST:
            ret = kSyscallUserWriteOptional_((UINT *)(UINTPTR)arg2,
                                             sizeof(UINT));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgQueueBroadcast((RK_MESG_QUEUE_HANDLE)(UINTPTR)arg0,
                                          (VOID *)(UINTPTR)arg1,
                                          (UINT *)(UINTPTR)arg2);
            }
            break;

        case RK_SYSCALL_MESG_QUEUE_BROADCAST_RECV:
            ret = kMesgQueueBroadcastRecvSyscall(
                framePtr, (RK_MESG_QUEUE_HANDLE)(UINTPTR)arg0,
                (VOID *)(UINTPTR)arg1, (RK_TICK)arg2);
            break;

        case RK_SYSCALL_MESG_QUEUE_CREATE:
        {
            ULONG bufWords = 0UL;
            ULONG bufBytes = 0UL;

            ret = kSyscallUserWriteRequired_(
                (RK_MESG_QUEUE_HANDLE *)(UINTPTR)arg0,
                sizeof(RK_MESG_QUEUE_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(arg2, arg3, &bufWords);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(bufWords, RK_WORD_SIZE, &bufBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_((VOID *)(UINTPTR)arg1,
                                                 bufBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgQueueCreate((RK_MESG_QUEUE_HANDLE *)(UINTPTR)arg0,
                                       (VOID *)(UINTPTR)arg1, arg2, arg3);
            }
            break;
        }

        case RK_SYSCALL_MESG_QUEUE_DESTROY:
            ret = kSyscallUserWriteRequired_(
                (RK_MESG_QUEUE_HANDLE *)(UINTPTR)arg0,
                sizeof(RK_MESG_QUEUE_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgQueueDestroy((RK_MESG_QUEUE_HANDLE *)(UINTPTR)arg0);
            }
            break;
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
        /* Asynchronous direct messages. */
        case RK_SYSCALL_MESG_ENDPOINT_INIT:
            ret = kMesgEndpointInit((RK_TASK_HANDLE)(UINTPTR)arg0);
            break;

        case RK_SYSCALL_MESG_POOL_INIT:
        {
            RK_MESG_POOL_INIT_SYSCALL_ARGS args;
            ULONG blockBytes = 0UL;
            ULONG poolBytes = 0UL;

            ret = kSyscallUserStructCopy_((VOID const *)(UINTPTR)arg0,
                                          &args, sizeof(args));
            if (ret == RK_ERR_SUCCESS)
            {
                if ((args.payloadBytes == 0UL) ||
                    (args.payloadBytes >
                     (RK_ULONG_MAX - (ULONG)sizeof(RK_MESG))))
                {
                    ret = RK_ERR_INVALID_PARAM;
                }
                else
                {
                    blockBytes = (ULONG)sizeof(RK_MESG) + args.payloadBytes;
                    if (blockBytes > (RK_ULONG_MAX - (RK_WORD_SIZE - 1UL)))
                    {
                        ret = RK_ERR_INVALID_PARAM;
                    }
                    else
                    {
                        blockBytes = (blockBytes + RK_WORD_SIZE - 1UL) &
                                     ~(RK_WORD_SIZE - 1UL);
                    }
                }
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(blockBytes, args.nMesg, &poolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.poolPtr,
                                                 sizeof(RK_MEM_PARTITION));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.memPoolPtr, poolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgPoolInit(args.poolPtr, args.memPoolPtr,
                                    args.payloadBytes, args.nMesg,
                                    args.ceilingPrio);
            }
            break;
        }

        case RK_SYSCALL_MESG_ALLOC:
            ret = kSyscallUserWriteRequired_((RK_MESG **)(UINTPTR)arg1,
                                             sizeof(RK_MESG *));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgAllocSyscall(framePtr,
                                        (RK_MEM_PARTITION *)(UINTPTR)arg0,
                                        (RK_MESG **)(UINTPTR)arg1,
                                        (RK_TICK)arg2);
            }
            break;

        case RK_SYSCALL_MESG_FREE:
            ret = kSyscallUserWriteRequired_((RK_MESG *)(UINTPTR)arg0,
                                             sizeof(RK_MESG));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgFree((RK_MESG *)(UINTPTR)arg0);
            }
            break;

        case RK_SYSCALL_MESG_PAYLOAD:
            if (kSyscallUserReadRequired_((RK_MESG const *)(UINTPTR)arg0,
                                          sizeof(RK_MESG)) != RK_ERR_SUCCESS)
            {
                framePtr->r0 = 0UL;
            }
            else
            {
                framePtr->r0 =
                    (ULONG)(UINTPTR)kMesgPayload((RK_MESG *)(UINTPTR)arg0);
            }
            return;

        case RK_SYSCALL_MESG_PAYLOAD_CONST:
            if (kSyscallUserReadRequired_((RK_MESG const *)(UINTPTR)arg0,
                                          sizeof(RK_MESG)) != RK_ERR_SUCCESS)
            {
                framePtr->r0 = 0UL;
            }
            else
            {
                framePtr->r0 =
                    (ULONG)(UINTPTR)kMesgPayloadConst(
                        (RK_MESG const *)(UINTPTR)arg0);
            }
            return;

        case RK_SYSCALL_MESG_PAYLOAD_BYTES:
            if (kSyscallUserReadRequired_((RK_MESG const *)(UINTPTR)arg0,
                                          sizeof(RK_MESG)) != RK_ERR_SUCCESS)
            {
                framePtr->r0 = 0UL;
            }
            else
            {
                framePtr->r0 =
                    (ULONG)kMesgPayloadBytes((RK_MESG const *)(UINTPTR)arg0);
            }
            return;

        case RK_SYSCALL_MESG_GET_SENDER_HANDLE:
            if (kSyscallUserReadRequired_((RK_MESG const *)(UINTPTR)arg0,
                                          sizeof(RK_MESG)) != RK_ERR_SUCCESS)
            {
                framePtr->r0 = 0UL;
            }
            else
            {
                framePtr->r0 =
                    (ULONG)(UINTPTR)kMesgGetSenderHandle(
                        (RK_MESG const *)(UINTPTR)arg0);
            }
            return;

        case RK_SYSCALL_MESG_GET_SENDER_ID:
            ret = kSyscallUserReadRequired_((RK_MESG const *)(UINTPTR)arg0,
                                            sizeof(RK_MESG));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_((RK_TID *)(UINTPTR)arg1,
                                                 sizeof(RK_TID));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgGetSenderID((RK_MESG const *)(UINTPTR)arg0,
                                       (RK_TID *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_MESG_SEND:
            ret = kSyscallUserWriteRequired_((RK_MESG *)(UINTPTR)arg1,
                                             sizeof(RK_MESG));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgSend((RK_TASK_HANDLE)(UINTPTR)arg0,
                                (RK_MESG *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_MESG_WAIT:
            ret = kSyscallUserWriteRequired_((RK_MESG **)(UINTPTR)arg1,
                                             sizeof(RK_MESG *));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgWaitSyscall(framePtr,
                                       (RK_TASK_HANDLE)(UINTPTR)arg0,
                                       (RK_MESG **)(UINTPTR)arg1,
                                       (RK_TICK)arg2);
            }
            break;

#if (RK_CONF_ASYNCH_COPY_MESG == ON)
        case RK_SYSCALL_MESG_COPY_ENDPOINT_INIT:
            ret = kMesgCopyEndpointInit((RK_TASK_HANDLE)(UINTPTR)arg0);
            break;

        case RK_SYSCALL_MESG_SEND_COPY:
            if ((arg2 == 0UL) ||
                (arg2 > RK_CONF_ASYNCH_COPY_MESG_PAYLOAD_BYTES))
            {
                ret = RK_ERR_INVALID_MSG_SIZE;
            }
            else
            {
                ret = kSyscallUserReadRequired_((VOID const *)(UINTPTR)arg1,
                                                arg2);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgSendCopy((RK_TASK_HANDLE)(UINTPTR)arg0,
                                    (VOID const *)(UINTPTR)arg1, arg2);
            }
            break;

        case RK_SYSCALL_MESG_RECV_COPY:
        {
            RK_MESG_RECV_COPY_SYSCALL_ARGS args;

            ret = kSyscallUserStructCopy_((VOID const *)(UINTPTR)arg0,
                                          &args, sizeof(args));
            if ((ret == RK_ERR_SUCCESS) && (args.recvBytes == 0UL))
            {
                ret = RK_ERR_INVALID_MSG_SIZE;
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.recvPtr,
                                                 args.recvBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteOptional_(
                    args.rxBytesPtr, sizeof(ULONG));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMesgRecvCopySyscall(framePtr, arg0, &args);
            }
            break;
        }
#endif
#endif

#if (RK_CONF_SYNCH_MESG == ON)
        /* Synchronous direct messages. */
        case RK_SYSCALL_SYNCH_MESG_INIT:
            ret = kSynchMesgInit((RK_TASK_HANDLE)(UINTPTR)arg0, arg1);
            break;

        case RK_SYSCALL_SYNCH_SEND_WAIT:
            ret = kSynchSendWaitSyscall(framePtr,
                                        (RK_TASK_HANDLE)(UINTPTR)arg0,
                                        (VOID const *)(UINTPTR)arg1,
                                        arg2, (RK_TICK)arg3);
            break;

        case RK_SYSCALL_SYNCH_RECV:
            ret = kSyncRecvSyscall(framePtr, (VOID *)(UINTPTR)arg0,
                                   (ULONG *)(UINTPTR)arg1,
                                   (RK_TICK)arg2);
            break;

        case RK_SYSCALL_SYNCH_MESG_CALL:
            ret = kSynchMesgCallSyscall(
                framePtr, (RK_TASK_HANDLE)(UINTPTR)arg0,
                (RK_SYNCH_ATTR const *)(UINTPTR)arg1, (RK_TICK)arg2);
            break;

        case RK_SYSCALL_SYNCH_MESG_ACCEPT:
            ret = kSynchMesgAcceptSyscall(
                framePtr, (RK_SYNCH_CALL_DATA *)(UINTPTR)arg0,
                (VOID *)(UINTPTR)arg1, (ULONG *)(UINTPTR)arg2,
                (RK_TICK)arg3);
            break;

        case RK_SYSCALL_SYNCH_MESG_REPLY:
            ret = kSyscallUserReadRequired_(
                (RK_SYNCH_CALL_DATA const *)(UINTPTR)arg0,
                sizeof(RK_SYNCH_CALL_DATA));
            if ((ret == RK_ERR_SUCCESS) && (arg2 > 0UL))
            {
                ret = kSyscallUserReadRequired_((VOID const *)(UINTPTR)arg1,
                                                arg2);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSynchMesgReply(
                    (RK_SYNCH_CALL_DATA const *)(UINTPTR)arg0,
                    (VOID const *)(UINTPTR)arg1, arg2);
            }
            break;
#endif

#if (RK_CONF_MRM == ON)
        /* Multi-reader mailbox buffers. */
        case RK_SYSCALL_MRM_INIT:
        {
            RK_MRM_INIT_SYSCALL_ARGS args;
            ULONG mrmPoolBytes = 0UL;
            ULONG dataWords = 0UL;
            ULONG dataPoolBytes = 0UL;

            ret = kSyscallUserStructCopy_((VOID const *)(UINTPTR)arg0,
                                          &args, sizeof(args));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(args.nBufs,
                                       (ULONG)sizeof(RK_MRM_BUF),
                                       &mrmPoolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(args.nBufs, args.dataSizeWords,
                                       &dataWords);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(dataWords, RK_WORD_SIZE,
                                       &dataPoolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.mrmPtr,
                                                 sizeof(RK_MRM));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.mrmPoolPtr,
                                                 mrmPoolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.mesgPoolPtr,
                                                 dataPoolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMRMInit(args.mrmPtr, args.mrmPoolPtr,
                               args.mesgPoolPtr, args.nBufs,
                               args.dataSizeWords);
            }
            break;
        }

        case RK_SYSCALL_MRM_CREATE:
        {
            RK_MRM_CREATE_SYSCALL_ARGS args;
            ULONG mrmPoolBytes = 0UL;
            ULONG dataWords = 0UL;
            ULONG dataPoolBytes = 0UL;

            ret = kSyscallUserStructCopy_((VOID const *)(UINTPTR)arg0,
                                          &args, sizeof(args));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.mrmHandlePtr,
                                                 sizeof(RK_MRM_HANDLE));
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(args.nBufs,
                                       (ULONG)sizeof(RK_MRM_BUF),
                                       &mrmPoolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(args.nBufs, args.dataSizeWords,
                                       &dataWords);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallSizeMul_(dataWords, RK_WORD_SIZE,
                                       &dataPoolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.mrmPoolPtr,
                                                 mrmPoolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kSyscallUserWriteRequired_(args.mesgPoolPtr,
                                                 dataPoolBytes);
            }
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMRMCreate(args.mrmHandlePtr, args.mrmPoolPtr,
                                 args.mesgPoolPtr, args.nBufs,
                                 args.dataSizeWords);
            }
            break;
        }

        case RK_SYSCALL_MRM_DESTROY:
            ret = kSyscallUserWriteRequired_((RK_MRM_HANDLE *)(UINTPTR)arg0,
                                             sizeof(RK_MRM_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kMRMDestroy((RK_MRM_HANDLE *)(UINTPTR)arg0);
            }
            break;

        case RK_SYSCALL_MRM_RESERVE:
            framePtr->r0 =
                (ULONG)(UINTPTR)kMRMReserve((RK_MRM_HANDLE)(UINTPTR)arg0);
            return;

        case RK_SYSCALL_MRM_PUBLISH:
            ret = kMRMPublish((RK_MRM_HANDLE)(UINTPTR)arg0,
                              (RK_MRM_BUF *)(UINTPTR)arg1,
                              (VOID const *)(UINTPTR)arg2);
            break;

        case RK_SYSCALL_MRM_GET:
            framePtr->r0 =
                (ULONG)(UINTPTR)kMRMGet((RK_MRM_HANDLE)(UINTPTR)arg0,
                                        (VOID *)(UINTPTR)arg1);
            return;

        case RK_SYSCALL_MRM_UNGET:
            ret = kMRMUnget((RK_MRM_HANDLE)(UINTPTR)arg0,
                            (RK_MRM_BUF *)(UINTPTR)arg1);
            break;
#endif

#if (RK_CONF_CALLOUT_TIMER == ON)
        /* Callout timers. */
        case RK_SYSCALL_TIMER_INIT:
            ret = RK_ERR_INVALID_PHASE;
            break;

        case RK_SYSCALL_TIMER_CREATE:
            ret = RK_ERR_INVALID_PHASE;
            break;

        case RK_SYSCALL_TIMER_DESTROY:
            ret = kSyscallUserWriteRequired_((RK_TIMER_HANDLE *)(UINTPTR)arg0,
                                             sizeof(RK_TIMER_HANDLE));
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kTimerDestroy((RK_TIMER_HANDLE *)(UINTPTR)arg0);
            }
            break;

        case RK_SYSCALL_TIMER_CANCEL:
            ret = kTimerCancel((RK_TIMER_HANDLE)(UINTPTR)arg0);
            break;

        case RK_SYSCALL_TIMER_RELOAD:
            kTimerReload((RK_TIMER_HANDLE)(UINTPTR)arg0, (RK_TICK)arg1);
            ret = RK_ERR_SUCCESS;
            break;
#endif

#if (RK_CONF_SYSMON == ON)
        case RK_SYSCALL_SYSMON_COMMAND:
            if (arg1 >= (ULONG)RK_CONF_SYSMON_LINE_LEN)
            {
                ret = RK_ERR_INVALID_PARAM;
            }
            else
            {
                ret = kSyscallUserReadRequired_((CHAR const *)(UINTPTR)arg0,
                                                arg1);
                if (ret == RK_ERR_SUCCESS)
                {
                    ret = kSysMonCommand((CHAR const *)(UINTPTR)arg0, arg1);
                }
            }
            break;
#endif

#if (RK_CONF_TRACE == ON)
        /* Trace console and snapshot services. */
        case RK_SYSCALL_TRACE_INIT:
            ret = kTraceInit();
            break;

        case RK_SYSCALL_TRACE_POLL:
            kTracePoll();
            ret = RK_ERR_SUCCESS;
            break;

        case RK_SYSCALL_TRACE_INPUT_SIGNAL:
            kTraceInputSignalFromISR();
            ret = RK_ERR_SUCCESS;
            break;

        case RK_SYSCALL_TRACE_OBJECT_NAME_SET:
            ret = kSyscallUserReadRequired_((CHAR const *)(UINTPTR)arg1,
                                            RK_NAME_SIZE);
            if (ret == RK_ERR_SUCCESS)
            {
                ret = kTraceObjectNameSet((RK_HANDLE)arg0,
                                          (CHAR const *)(UINTPTR)arg1);
            }
            break;

        case RK_SYSCALL_TRACE_RECORD_OBJECT:
            kTraceRecordObject((VOID *)(UINTPTR)arg0,
                               (RK_TRACE_OP)arg1,
                               (RK_ERR)arg2,
                               arg3);
            ret = RK_ERR_SUCCESS;
            break;

        case RK_SYSCALL_TRACE_RECORD_TASK_PRIO:
            kTraceRecordTaskPrio((RK_TASK_HANDLE)(UINTPTR)arg0,
                                 (RK_PRIO)arg1, (RK_PRIO)arg2);
            ret = RK_ERR_SUCCESS;
            break;

        case RK_SYSCALL_TRACE_RECORD_TASK_OVERRUN:
            kTraceRecordTaskOverrun((RK_TRACE_OVERRUN_KIND)arg0,
                                    (RK_TICK)arg1, (RK_TICK)arg2,
                                    arg3);
            ret = RK_ERR_SUCCESS;
            break;

        case RK_SYSCALL_TRACE_TASK_SNAPSHOT:
        {
            ULONG bytes = 0UL;

            if ((kSyscallSizeMul_(arg1, (ULONG)sizeof(RK_TRACE_TASK_INFO),
                                  &bytes) != RK_ERR_SUCCESS) ||
                (kSyscallUserWriteRequired_(
                     (RK_TRACE_TASK_INFO *)(UINTPTR)arg0,
                     bytes) != RK_ERR_SUCCESS))
            {
                framePtr->r0 = 0UL;
            }
            else
            {
                framePtr->r0 =
                    (ULONG)kTraceTaskSnapshot(
                        (RK_TRACE_TASK_INFO *)(UINTPTR)arg0,
                        (UINT)arg1);
            }
            return;
        }

#if (RK_CONF_MESG_QUEUE == ON)
        case RK_SYSCALL_TRACE_MESG_SNAPSHOT:
        {
            ULONG bytes = 0UL;

            if ((kSyscallSizeMul_(arg1, (ULONG)sizeof(RK_TRACE_OBJECT_INFO),
                                  &bytes) != RK_ERR_SUCCESS) ||
                (kSyscallUserWriteRequired_(
                     (RK_TRACE_OBJECT_INFO *)(UINTPTR)arg0,
                     bytes) != RK_ERR_SUCCESS))
            {
                framePtr->r0 = 0UL;
            }
            else
            {
                framePtr->r0 =
                    (ULONG)kTraceMesgSnapshot(
                        (RK_TRACE_OBJECT_INFO *)(UINTPTR)arg0,
                        (UINT)arg1);
            }
            return;
        }
#endif

#if ((RK_CONF_SEMAPHORE == ON) || (RK_CONF_MUTEX == ON))
        case RK_SYSCALL_TRACE_SEMA_SNAPSHOT:
        {
            ULONG bytes = 0UL;

            if ((kSyscallSizeMul_(arg1, (ULONG)sizeof(RK_TRACE_SYNC_INFO),
                                  &bytes) != RK_ERR_SUCCESS) ||
                (kSyscallUserWriteRequired_(
                     (RK_TRACE_SYNC_INFO *)(UINTPTR)arg0,
                     bytes) != RK_ERR_SUCCESS))
            {
                framePtr->r0 = 0UL;
            }
            else
            {
                framePtr->r0 =
                    (ULONG)kTraceSemaSnapshot(
                        (RK_TRACE_SYNC_INFO *)(UINTPTR)arg0,
                        (UINT)arg1);
            }
            return;
        }
#endif

#if (RK_CONF_CALLOUT_TIMER == ON)
        case RK_SYSCALL_TRACE_TIMER_SNAPSHOT:
        {
            ULONG bytes = 0UL;

            if ((kSyscallSizeMul_(arg1, (ULONG)sizeof(RK_TRACE_TIMER_INFO),
                                  &bytes) != RK_ERR_SUCCESS) ||
                (kSyscallUserWriteRequired_(
                     (RK_TRACE_TIMER_INFO *)(UINTPTR)arg0,
                     bytes) != RK_ERR_SUCCESS))
            {
                framePtr->r0 = 0UL;
            }
            else
            {
                framePtr->r0 =
                    (ULONG)kTraceTimerSnapshot(
                        (RK_TRACE_TIMER_INFO *)(UINTPTR)arg0,
                        (UINT)arg1);
            }
            return;
        }
#endif

        case RK_SYSCALL_TRACE_RECORD_SNAPSHOT:
        {
            ULONG bytes = 0UL;

            if ((kSyscallSizeMul_(arg2, (ULONG)sizeof(RK_TRACE_RECORD_INFO),
                                  &bytes) != RK_ERR_SUCCESS) ||
                (kSyscallUserWriteRequired_(
                     (RK_TRACE_RECORD_INFO *)(UINTPTR)arg1,
                     bytes) != RK_ERR_SUCCESS))
            {
                framePtr->r0 = 0UL;
            }
            else
            {
                framePtr->r0 =
                    (ULONG)kTraceRecordSnapshot(
                        (RK_HANDLE)arg0,
                        (RK_TRACE_RECORD_INFO *)(UINTPTR)arg1,
                        (UINT)arg2);
            }
            return;
        }

        case RK_SYSCALL_TRACE_TASK_PRIO_SNAPSHOT:
        {
            ULONG bytes = 0UL;

            if ((kSyscallSizeMul_(
                     arg2, (ULONG)sizeof(RK_TRACE_PRIO_RECORD_INFO),
                     &bytes) != RK_ERR_SUCCESS) ||
                (kSyscallUserWriteRequired_(
                     (RK_TRACE_PRIO_RECORD_INFO *)(UINTPTR)arg1,
                     bytes) != RK_ERR_SUCCESS))
            {
                framePtr->r0 = 0UL;
            }
            else
            {
                framePtr->r0 =
                    (ULONG)kTraceTaskPrioSnapshot(
                        (RK_TASK_HANDLE)(UINTPTR)arg0,
                        (RK_TRACE_PRIO_RECORD_INFO *)(UINTPTR)arg1,
                        (UINT)arg2);
            }
            return;
        }

        case RK_SYSCALL_TRACE_REGISTER_OBJECT:
            kTraceRegisterObject((VOID *)(UINTPTR)arg0, (RK_ID)arg1);
            ret = RK_ERR_SUCCESS;
            break;

        case RK_SYSCALL_TRACE_UNREGISTER_OBJECT:
            kTraceUnregisterObject((VOID *)(UINTPTR)arg0);
            ret = RK_ERR_SUCCESS;
            break;

        case RK_SYSCALL_TRACE_OVERFLOW_PERSIST:
            ret = kSyscallUserReadRequired_(
                (RK_TRACE_OVERFLOW_INFO const *)(UINTPTR)arg0,
                sizeof(RK_TRACE_OVERFLOW_INFO));
            if (ret == RK_ERR_SUCCESS)
            {
                kTraceOverflowPersist(
                    (RK_TRACE_OVERFLOW_INFO const *)(UINTPTR)arg0);
            }
            break;
#endif

        /* Miscellaneous public services. */
        case RK_SYSCALL_CONSOLE_WRITE:
            ret = kConsoleWriteSyscall(
                framePtr, (RK_SYNCH_ATTR const *)(UINTPTR)arg0,
                (RK_TICK)arg1);
            break;

        case RK_SYSCALL_GET_VERSION:
            framePtr->r0 = (ULONG)kGetVersion();
            return;

        case RK_SYSCALL_ERR_HANDLER:
            kErrHandler((RK_FAULT)arg0);
            ret = RK_ERR_SUCCESS;
            break;

#if (RK_CONF_SVC_DEFER_TEST == ON)
        case RK_SYSCALL_TEST_SYSTICK_DEFER:
            ret = kSyscallTestSysTickDefer_(framePtr, arg0);
            break;
#endif

        default:
            /* Give split-out core services and application-level syscall
             * extensions a chance before rejecting an unknown number. */
            if (kLoggerSyscallDispatch(framePtr, callNumber, arg0, arg1,
                                       arg2, arg3) == RK_TRUE)
            {
                return;
            }

            if (kSyscallDispatchApp(framePtr, callNumber, arg0, arg1,
                                    arg2, arg3) == RK_TRUE)
            {
                return;
            }

            ret = RK_ERR_INVALID_PARAM;
            break;
    }

    if (ret != RK_ERR_SYSCALL_RESTART)
    {
        kSyscallSetFrameReturn_(framePtr, ret);
    }
}

VOID kSyscallDispatch(RK_EXCEPTION_FRAME *const framePtr,
                      ULONG const excReturn,
                      ULONG const svcNumber)
{
    if (framePtr == NULL)
    {
        return;
    }

    /* Only the dedicated RK01 SVC immediate is accepted, and only when the SVC
     * came from thread mode. */
    if ((svcNumber != RK_SVC_SYSCALL_IMM) ||
        (kSyscallThreadOrigin_(excReturn) == RK_FALSE))
    {
        kSyscallSetFrameReturn_(framePtr, RK_ERR_INVALID_PARAM);
        return;
    }

    /* The syscall already runs in handler mode, so task-level execution cannot
     * preempt it until SVC returns. SysTick may still run above SVC and service
     * timeout state immediately; any resulting PendSV naturally remains pending
     * until exception return. */
    RK_gSyscallThreadModeActive++;
    kSyscallDispatchActive_(framePtr);
    kTickDrainDeferredOnSyscallExit();
}
