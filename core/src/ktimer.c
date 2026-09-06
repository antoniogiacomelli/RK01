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
 *   Kernel tick, sleep timeouts and callout timers. The timer list is bounded
 *   and services scheduler wakeups, blocking syscall timeouts and application
 *   timer callbacks.
 *
 * Contracts/invariants:
 *   - Armed timeout nodes belong to exactly one timeout list.
 *   - The timeout list is a delta list: each dtick is relative to the previous
 *     node.
 *   - Removing a node preserves absolute expiry for all later nodes.
 *   - Sleep-until accounting uses absolute wakeTime and overrunCount.
 */

#define RK_SOURCE_CODE
#include <kdynobjs.h>
#include "ktimer.h"
#include <ksch.h>
#include <ksyscall.h>
#include <ktrace.h>

#if (RK_CONF_MUTEX == ON)
extern VOID kMutexTimeoutWaiter(RK_TCB *const waiterPtr);
#endif
#if (RK_CONF_SYNCH_MESG == ON)
extern VOID kSynchMesgTimeoutSend(RK_TCB *const senderPtr);
extern VOID kSynchMesgTimeoutCall(RK_TCB *const callerPtr);
#endif

/******************************************************************************
 * GLOBAL TICK RETURN
 *****************************************************************************/

ULONG RK_gSysTickInterval = 0;

#ifndef RK_BUSY_DELAY_LOOP_CYCLES
#define RK_BUSY_DELAY_LOOP_CYCLES (4UL)
#endif

#if defined(RK_CONF_EFFECTIVE_SYSCORECLK) && (RK_CONF_EFFECTIVE_SYSCORECLK > 0UL)
#define RK_BUSY_DELAY_RAW_LOOPS_PER_TICK                                      \
    ((RK_CONF_EFFECTIVE_SYSCORECLK / RK_CONF_SYSTICK_DIV) /                 \
     RK_BUSY_DELAY_LOOP_CYCLES)
#else
#define RK_BUSY_DELAY_RAW_LOOPS_PER_TICK (1UL)
#endif

#if (RK_BUSY_DELAY_RAW_LOOPS_PER_TICK == 0UL)
#define RK_BUSY_DELAY_LOOPS_PER_TICK (1UL)
#else
#define RK_BUSY_DELAY_LOOPS_PER_TICK RK_BUSY_DELAY_RAW_LOOPS_PER_TICK
#endif

/*
 * Busy-delay is deliberately approximate. It consumes thread-mode time without
 * touching scheduler state, and the loop calibration is a compile-time bound
 * used only to avoid entering SVC for a pure spin delay.
 */
static VOID kBusyDelaySpin_(ULONG loops)
{
    while (loops != 0UL)
    {
        RK_NOP
        loops--;
    }
}

RK_TICK kTickGet(void)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_TICK)kSyscallInvoke4(RK_SYSCALL_TICK_GET, 0UL, 0UL,
                                         0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER
    RK_TICK ret = (RK_gRunTime.globalTick);
    RK_CR_EXIT
    return (ret);
}

RK_TICK kTickGetMs(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_TICK)kSyscallInvoke4(RK_SYSCALL_TICK_GET_MS, 0UL, 0UL,
                                         0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER
    RK_TICK ret = 0UL;
    if (RK_gSysTickInterval != 0)
    {
        ret = (RK_gRunTime.globalTick * RK_gSysTickInterval);
    }
    else
    {
        K_PANIC("System tick interval not set");
    }
    RK_CR_EXIT
    return (ret);
}

RK_BOOL kTimeoutNodeIsArmed(RK_TIMEOUT_NODE const *node)
{
    return (((node != NULL) && (node->listRefPtr != NULL)) ? RK_TRUE
                                                           : RK_FALSE);
}

/*
 * A reset timeout node is unlinked, has no list owner and carries no stale wait
 * metadata. Callers must reset after any path that removes or abandons a wait.
 */
VOID kTimeoutNodeReset(RK_TIMEOUT_NODE *node)
{
    if (node == NULL)
    {
        return;
    }

    node->nextPtr = NULL;
    node->prevPtr = NULL;
    node->listRefPtr = NULL;
    node->timeoutType = 0U;
    node->timeout = 0UL;
    node->dtick = 0UL;
    node->waitingQueuePtr = NULL;
    node->waitInfo = 0U;
}

RK_ERR kTimeoutNodeDisarm(RK_TIMEOUT_NODE *node)
{
    RK_ERR err = kRemoveTimeoutNode(node);

    if (err != RK_ERR_SUCCESS)
    {
        K_PANIC("Timeout handling critical failure");
        kTimeoutNodeReset(node);
        return (err);
    }

    kTimeoutNodeReset(node);
    return (RK_ERR_SUCCESS);
}

static RK_ERR kTimerReportErr_(RK_ERR const err)
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
    else if (err == RK_ERR_OBJ_DOUBLE_INIT)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_DOUBLE_INIT);
    }
    else if (err == RK_ERR_INVALID_PARAM)
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
#else
    (VOID)err;
#endif
    return (err);
}

static RK_ERR kTimerRunningTaskErr_(VOID)
{
    if (kIsISR())
    {
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((RK_gRunPtr == NULL) || (RK_gRunPtr->status != RK_RUNNING))
    {
        return (RK_ERR_TASK_INVALID_ST);
    }

    return (RK_ERR_SUCCESS);
}

RK_ERR kDelay(RK_TICK const ticks)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_DELAY, (ULONG)ticks,
                                        0UL, 0UL, 0UL));
    }

    if (kIsISR())
    {
        return (kTimerReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }
    if (ticks > RK_MAX_PERIOD)
    {
        return (kTimerReportErr_(RK_ERR_INVALID_PARAM));
    }

    for (RK_TICK remaining = ticks; remaining > 0UL; remaining--)
    {
        kBusyDelaySpin_(RK_BUSY_DELAY_LOOPS_PER_TICK);
    }

    return (RK_ERR_SUCCESS);
}

#if (RK_CONF_CALLOUT_TIMER == ON)

static RK_ERR kTimerResolve_(RK_TIMER_HANDLE const timerHandle,
                             RK_TIMER **const timerPPtr)
{
    if (timerPPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    *timerPPtr = NULL;

    VOID *objPtr = NULL;
    RK_ERR const err =
        kDynObjResolveHandle(RK_DYN_OBJ_TYPE_TIMER, timerHandle, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
    *timerPPtr = (RK_TIMER *)objPtr;
    return (RK_ERR_SUCCESS);
}

static RK_ERR kTimerReadyErr_(RK_TIMER const *const kobj)
{
    RK_ERR const readyErr =
        kObjHeaderReadyErr((kobj != NULL) ? &kobj->header : NULL,
                           RK_TIMER_KOBJ_ID);
    if (readyErr != RK_ERR_SUCCESS)
    {
        return (readyErr);
    }

    return (kObjHeaderDomainLocalAccessErr(
        &kobj->header, (RK_gRunPtr != NULL) ? RK_gRunPtr->domainPtr : NULL));
}

/******************************************************************************
 * CALLOUT TIMERS
 *****************************************************************************/
/*
 * Timer initialisation inserts the timer's timeout node immediately. The node
 * carries both the relative delay used by the delta list and the absolute
 * nextTime used by trace/query code.
 */
static inline RK_ERR kTimerListAdd_(RK_TIMER *kobj, RK_TICK phase,
                                    RK_TICK countTicks, RK_TIMER_CALLOUT funPtr,
                                    VOID *argsPtr, RK_OPTION reload)
{
    RK_TICK const initialDelay = phase + countTicks;

    kobj->timeoutNode.dtick = initialDelay;
    kobj->timeoutNode.timeout = initialDelay;
    kobj->timeoutNode.timeoutType = RK_TIMEOUT_CALL;
    kobj->funPtr = funPtr;
    kobj->argsPtr = argsPtr;
    kobj->reload = reload;
    kobj->phase = phase;
    kobj->period = countTicks;
    kobj->nextTime = K_TICK_ADD(kTickGet(), phase + countTicks);
    return (kTimeoutNodeAdd(&kobj->timeoutNode, initialDelay));
}

RK_ERR kTimerInit(RK_TIMER *const kobj, RK_TICK const phase,
                  RK_TICK const countTicks, RK_TIMER_CALLOUT const funPtr,
                  VOID *const argsPtr, RK_OPTION const reload)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        RK_TIMER_INIT_SYSCALL_ARGS syscallArgs;

        syscallArgs.timerPtr = kobj;
        syscallArgs.phase = phase;
        syscallArgs.countTicks = countTicks;
        syscallArgs.funPtr = funPtr;
        syscallArgs.argsPtr = argsPtr;
        syscallArgs.reload = reload;

        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TIMER_INIT,
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

    if ((kobj == NULL) || (funPtr == NULL))
    {
        RK_CR_EXIT
        return (kTimerReportErr_(RK_ERR_OBJ_NULL));
    }
    if (kobj->init == RK_TRUE)
    {
        RK_CR_EXIT
        return (kTimerReportErr_(RK_ERR_OBJ_DOUBLE_INIT));
    }

    if ((countTicks == 0UL) || (countTicks > RK_MAX_PERIOD) ||
        (phase > RK_MAX_PERIOD) ||
        (phase > (RK_MAX_PERIOD - countTicks)) ||
        ((reload != RK_TIMER_ONESHOT) && (reload != RK_TIMER_RELOAD)))
    {
        RK_CR_EXIT
        return (kTimerReportErr_(RK_ERR_INVALID_PARAM));
    }

    RK_ERR err =
        kTimerListAdd_(kobj, phase, countTicks, funPtr, argsPtr, reload);
    if (err == 0)
    {
        kobj->init = RK_TRUE;
        kobj->objID = RK_TIMER_KOBJ_ID;
        kobj->objName[0] = '\0';
        kObjHeaderOwnerDomainSet(&kobj->header,
                                 (RK_gRunPtr != NULL) ? RK_gRunPtr->domainPtr
                                                      : NULL);
        kTraceRegisterObject(kobj, RK_TIMER_KOBJ_ID);
    }
    RK_CR_EXIT
    return (err);
}

VOID kTimerReload(RK_TIMER_HANDLE const timerHandle, RK_TICK delay)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        (VOID)kSyscallInvoke4(RK_SYSCALL_TIMER_RELOAD,
                              (ULONG)(UINTPTR)timerHandle, (ULONG)delay,
                              0UL, 0UL);
        return;
    }

    RK_TIMER *kobj = NULL;
    RK_ERR const resolveErr = kTimerResolve_(timerHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
        (VOID)kTimerReportErr_(resolveErr);
        return;
    }

    RK_ERR const readyErr = kTimerReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        (VOID)kTimerReportErr_(readyErr);
        return;
    }

    kobj->timeoutNode.timeoutType = RK_TIMEOUT_CALL;
    RK_ERR const err = kTimeoutNodeAdd(&kobj->timeoutNode, delay);
    if (err != RK_ERR_SUCCESS)
    {
        (VOID)kTimerReportErr_(err);
        return;
    }
    K_ASSERT(err == RK_ERR_SUCCESS);
    kTraceRecordObject(kobj, RK_TRACE_OP_RELOAD, err, delay);
}

VOID kRemoveTimerNode(RK_TIMEOUT_NODE *node)
{
    RK_ERR err = kTimeoutNodeDisarm(node);
    K_ASSERT(err == RK_ERR_SUCCESS);
}

RK_ERR kTimerCancel(RK_TIMER_HANDLE const timerHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TIMER_CANCEL,
                                        (ULONG)(UINTPTR)timerHandle,
                                        0UL, 0UL, 0UL));
    }

    RK_TIMER *kobj = NULL;
    RK_ERR const resolveErr = kTimerResolve_(timerHandle, &kobj);
    if (resolveErr != RK_ERR_SUCCESS)
    {
        return (kTimerReportErr_(resolveErr));
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kTimerReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (kTimerReportErr_(readyErr));
    }

    RK_TIMEOUT_NODE *node = (RK_TIMEOUT_NODE *)&kobj->timeoutNode;
    RK_ERR err = RK_ERR_SUCCESS;
    if (kTimeoutNodeIsArmed(node) == RK_TRUE)
    {
        err = kTimeoutNodeDisarm(node);
    }
    kTraceRecordObject(kobj, RK_TRACE_OP_CANCEL, err, 0UL);

    RK_CR_EXIT
    return (err);
}
#endif
/*******************************************************************************
 * SLEEP TIMER AND BLOCKING TIME-OUT
 ******************************************************************************/
RK_ERR kSleepDelay(RK_TICK ticks)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SLEEP_DELAY, (ULONG)ticks,
                                        0UL, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kTimerRunningTaskErr_();
    if (readyErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (kTimerReportErr_(readyErr));
    }

    if (ticks > RK_MAX_PERIOD)
    {
        RK_CR_EXIT
        return (kTimerReportErr_(RK_ERR_INVALID_PARAM));
    }

    if (ticks == 0)
    {
        RK_CR_EXIT
        return (RK_ERR_TIMEOUT);
    }

    RK_TASK_SLEEP_TIMEOUT_SETUP

    RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, ticks);
    if (err != RK_ERR_SUCCESS)
    {
        kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
        RK_CR_EXIT
        return (err);
    }
    RK_gRunPtr->status = RK_SLEEPING_DELAY;
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSleepRelease(RK_TICK period)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SLEEP_RELEASE,
                                        (ULONG)period, 0UL, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kTimerRunningTaskErr_();
    if (readyErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (kTimerReportErr_(readyErr));
    }

    if ((period == 0UL) || ((ULONG)(period) > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        return (kTimerReportErr_(RK_ERR_INVALID_PARAM));
    }

    /* a call to sleep release means end cycle + schedule next activation */
    /* first waketime is 0, meaning the initial baseWake for every task is */
    /* when the schedular starts, so, on the first activation of every task */
    /* the accumulated delay until the end of its computation time is taken */
    /* into account. */
    /* offsetFactor is 1 for no overrun */
    /* then, offset is factor x period.    */
    /* delay = offset+current */

    RK_TICK current = kTickGet();
    RK_TICK baseWake = RK_gRunPtr->wakeTime;
    RK_TICK elapsed = K_TICK_DELTA(current, baseWake);

    /* elapsed/period is > 0 iff */
    RK_TICK offsetFactor = ((elapsed / period) + 1);
    if (offsetFactor > 1)
    {
        RK_gRunPtr->overrunCount += 1U;
        kTraceRecordTaskOverrun(RK_TRACE_OVERRUN_RELEASE, period,
                                (elapsed - period),
                                (ULONG)(offsetFactor - 1U));
    }
    RK_TICK offset = (RK_TICK)(offsetFactor * period);
    RK_TICK nextWake = K_TICK_ADD(baseWake, offset);
    RK_TICK delay = 0;
    delay = K_TICK_DELTA(nextWake, current);
    if (delay == 0)
    {
#ifndef NDEBUG

        {
            K_PANIC("0 DELAY SLEEPRELEASE\r\n");
            RK_CR_EXIT
            return (RK_ERR_ERROR);
        }
#else
        {
            /* this is not supposed to happen, but a possible fallback is
            to set reassign delay to remaining time next phase */

            /* get remainder */
            RK_TICK rem = current - (current / period) * period;
            delay = (rem == 0UL) ? period : (period - rem);
            nextWake = K_TICK_ADD(current, delay);
        }
#endif
    }
    RK_gRunPtr->wakeTime = nextWake;
    RK_TASK_SLEEP_TIMEOUT_SETUP
    RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, delay);
    if (err != RK_ERR_SUCCESS)
    {
        kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
        RK_CR_EXIT
        return (err);
    }
    RK_gRunPtr->status = RK_SLEEPING_RELEASE;
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

/* sleep for time, relative to local anchored tick */
RK_ERR kSleepUntil(RK_TICK *lastTickPtr, RK_TICK const ticks)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SLEEP_UNTIL, (ULONG)(UINTPTR)lastTickPtr,
            (ULONG)ticks, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

    if (lastTickPtr == NULL)
    {
        RK_CR_EXIT
        return kTimerReportErr_(RK_ERR_OBJ_NULL);
    }

    RK_ERR const readyErr = kTimerRunningTaskErr_();
    if (readyErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (kTimerReportErr_(readyErr));
    }

    if ((ticks == 0UL) || (ticks > RK_MAX_PERIOD))
    {
        RK_CR_EXIT
        return kTimerReportErr_(RK_ERR_INVALID_PARAM);
    }

    RK_TICK now = kTickGet();

    /* advance   */
    *lastTickPtr = K_TICK_ADD(*lastTickPtr, ticks);

    /* late or on-time  */
    if (kTickIsElapsed(*lastTickPtr, now))
    {
        RK_gRunPtr->overrunCount += 1U;
        kTraceRecordTaskOverrun(RK_TRACE_OVERRUN_UNTIL, ticks,
                                K_TICK_DELTA(now, *lastTickPtr), 0UL);
        RK_CR_EXIT
        return (RK_ERR_ELAPSED_PERIOD);
    }

    RK_TICK remaining = K_TICK_DELTA(*lastTickPtr, now);
    RK_TASK_SLEEP_TIMEOUT_SETUP
    RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, remaining);
    if (err != RK_ERR_SUCCESS)
    {
        kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
        RK_CR_EXIT
        return (err);
    }
    RK_gRunPtr->status = RK_SLEEPING_UNTIL;
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSleepDelaySyscall(RK_EXCEPTION_FRAME *const framePtr,
                          RK_TICK const ticks)
{
    RK_CR_AREA
    RK_CR_ENTER

    if (ticks > RK_MAX_PERIOD)
    {
        RK_CR_EXIT
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        return (RK_ERR_INVALID_PARAM);
    }

    if (ticks == 0UL)
    {
        RK_CR_EXIT
        return (RK_ERR_TIMEOUT);
    }

    RK_TASK_SLEEP_TIMEOUT_SETUP

    RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, ticks);
    if (err != RK_ERR_SUCCESS)
    {
        kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
        RK_CR_EXIT
        return (err);
    }

    RK_gRunPtr->status = RK_SLEEPING_DELAY;
    kSyscallTaskSuspend(framePtr, RK_SYSCALL_SLEEP_DELAY, (ULONG)ticks,
                        0UL, 0UL, 0UL, RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

RK_ERR kSleepReleaseSyscall(RK_EXCEPTION_FRAME *const framePtr,
                            RK_TICK const period)
{
    RK_CR_AREA
    RK_CR_ENTER

    if ((period == 0UL) || ((ULONG)period > RK_MAX_PERIOD))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    RK_TICK current = kTickGet();
    RK_TICK baseWake = RK_gRunPtr->wakeTime;
    RK_TICK elapsed = K_TICK_DELTA(current, baseWake);
    RK_TICK offsetFactor = ((elapsed / period) + 1U);

    if (offsetFactor > 1U)
    {
        RK_gRunPtr->overrunCount += 1U;
        kTraceRecordTaskOverrun(RK_TRACE_OVERRUN_RELEASE, period,
                                (elapsed - period),
                                (ULONG)(offsetFactor - 1U));
    }

    RK_TICK offset = (RK_TICK)(offsetFactor * period);
    RK_TICK nextWake = K_TICK_ADD(baseWake, offset);
    RK_TICK delay = K_TICK_DELTA(nextWake, current);

    if (delay == 0UL)
    {
#ifndef NDEBUG
        K_PANIC("0 DELAY SLEEPRELEASE\r\n");
        RK_CR_EXIT
        return (RK_ERR_ERROR);
#else
        RK_TICK rem = current - (current / period) * period;
        delay = (rem == 0UL) ? period : (period - rem);
        nextWake = K_TICK_ADD(current, delay);
#endif
    }

    RK_gRunPtr->wakeTime = nextWake;
    RK_TASK_SLEEP_TIMEOUT_SETUP

    RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, delay);
    if (err != RK_ERR_SUCCESS)
    {
        kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
        RK_CR_EXIT
        return (err);
    }

    RK_gRunPtr->status = RK_SLEEPING_RELEASE;
    kSyscallTaskSuspend(framePtr, RK_SYSCALL_SLEEP_RELEASE, (ULONG)period,
                        0UL, 0UL, 0UL, RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

RK_ERR kSleepUntilSyscall(RK_EXCEPTION_FRAME *const framePtr,
                          RK_TICK *const lastTickPtr,
                          RK_TICK const ticks)
{
    RK_CR_AREA
    RK_CR_ENTER

    if (lastTickPtr == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if ((ticks == 0UL) || (ticks > RK_MAX_PERIOD))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    RK_TICK now = kTickGet();
    *lastTickPtr = K_TICK_ADD(*lastTickPtr, ticks);

    if (kTickIsElapsed(*lastTickPtr, now))
    {
        RK_gRunPtr->overrunCount += 1U;
        kTraceRecordTaskOverrun(RK_TRACE_OVERRUN_UNTIL, ticks,
                                K_TICK_DELTA(now, *lastTickPtr), 0UL);
        RK_CR_EXIT
        return (RK_ERR_ELAPSED_PERIOD);
    }

    RK_TICK remaining = K_TICK_DELTA(*lastTickPtr, now);
    RK_TASK_SLEEP_TIMEOUT_SETUP

    RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, remaining);
    if (err != RK_ERR_SUCCESS)
    {
        kTimeoutNodeReset(&RK_gRunPtr->timeoutNode);
        RK_CR_EXIT
        return (err);
    }

    RK_gRunPtr->status = RK_SLEEPING_UNTIL;
    kSyscallTaskSuspend(framePtr, RK_SYSCALL_SLEEP_UNTIL,
                        (ULONG)(UINTPTR)lastTickPtr, (ULONG)ticks, 0UL, 0UL,
                        RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

/*
 * Delta-list invariant: each node's dtick is relative to the previous node,
 * and the sum from head to any node equals that node's remaining delay. Insert
 * consumes preceding deltas and subtracts the inserted delta from the successor.
 */
static void kTimeoutListInsertDelta_(RK_TIMEOUT_NODE **headPtr,
                                     RK_TIMEOUT_NODE *node)
{
    RK_TIMEOUT_NODE *currPtr = *headPtr;
    RK_TIMEOUT_NODE *prevPtr = NULL;

    while (currPtr != NULL && currPtr->dtick < node->dtick)
    {
        node->dtick -= currPtr->dtick;
        prevPtr = currPtr;
        currPtr = currPtr->nextPtr;
    }

    /* Link node in between prevPtr and currPtr */
    node->nextPtr = currPtr;
    node->prevPtr = prevPtr;

    if (currPtr != NULL)
    {
        currPtr->dtick -= node->dtick;
        currPtr->prevPtr = node;
    }

    if (prevPtr != NULL)
    {
        prevPtr->nextPtr = node;
    }
    else
    {
        *headPtr = node;
    }
}

/* Select the timeout list that owns this node type. */
RK_FORCE_INLINE static inline volatile RK_TIMEOUT_NODE **
kTimeoutNodeListRef_(RK_TIMEOUT_NODE *const node)
{
#if (RK_CONF_CALLOUT_TIMER == ON)
    if (node->timeoutType == RK_TIMEOUT_CALL)
    {
        return (&RK_gTimerListHeadPtr);
    }
#endif
    K_UNUSE(node);
    return (&RK_gTimeOutListHeadPtr);
}

/* Add a disarmed timeout node to the selected delta list. */
RK_ERR kTimeoutNodeAdd(RK_TIMEOUT_NODE *timeOutNode, RK_TICK timeout)
{
    if (timeout == 0)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
#endif
        return (RK_ERR_INVALID_TIMEOUT);
    }
    if (timeout > RK_MAX_PERIOD)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_TIMEOUT);
#endif
        return (RK_ERR_INVALID_TIMEOUT);
    }
    if (timeOutNode == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }
    if (timeOutNode->timeoutType == 0U)
    {
        K_PANIC("Timeout handling critical failure");
        kTimeoutNodeReset(timeOutNode);
        return (RK_ERR_ERROR);
    }
    if (kTimeoutNodeIsArmed(timeOutNode) != RK_FALSE)
    {
        K_PANIC("Timeout handling critical failure");
        kRemoveTimeoutNode(timeOutNode);
        kTimeoutNodeReset(timeOutNode);
        return (RK_ERR_ERROR);
    }
    if (timeOutNode->timeoutType != RK_TIMEOUT_CALL)
    {
        if (RK_gRunPtr == NULL)
        {
#if (RK_CONF_ERR_CHECK == ON)
            K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
            return (RK_ERR_OBJ_NULL);
        }
        RK_gRunPtr->timeOut = RK_FALSE;
    }
    timeOutNode->timeout = timeout;
    timeOutNode->prevPtr = NULL;
    timeOutNode->nextPtr = NULL;
    timeOutNode->dtick = timeout;
    timeOutNode->listRefPtr = kTimeoutNodeListRef_(timeOutNode);

    kTimeoutListInsertDelta_((RK_TIMEOUT_NODE **)timeOutNode->listRefPtr,
                             timeOutNode);

    return (RK_ERR_SUCCESS);
}

/*
 * Sleep-until waiters are ordered by absolute wake time. This queue is separate
 * from the delta timeout list because release-until needs overrun accounting.
 */
RK_FORCE_INLINE
static inline RK_ERR kTCBQEnqByWakeTime(RK_TCBQ *const kobj,
                                        RK_TCB *const tcbPtr)
{
    RK_NODE *currNodePtr = &(kobj->listDummy);

    while (currNodePtr->nextPtr != &(kobj->listDummy))
    {
        RK_TCB const *currTcbPtr = K_GET_TCB_ADDR(currNodePtr->nextPtr);
        if (currTcbPtr->wakeTime < tcbPtr->wakeTime)
        {
            break;
        }
        currNodePtr = currNodePtr->nextPtr;
    }

    RK_ERR err = kListInsertAfter(kobj, currNodePtr, &(tcbPtr->tcbNode));

    return (err);
}

/*
 * Ready the task associated with an expired timeout node. Each timeout type
 * owns its own cleanup contract before the task is moved back to the ready
 * queue.
 */
RK_ERR kTimeoutNodeReady(volatile RK_TIMEOUT_NODE *node)
{
    RK_TCB *taskPtr = K_GET_CONTAINER_ADDR(node, RK_TCB, timeoutNode);

    K_ASSERT(taskPtr != NULL);

    RK_ERR err = -1;

    if (taskPtr->timeoutNode.timeoutType == RK_TIMEOUT_BLOCKING)
    {

        err = kTCBQRem(taskPtr->timeoutNode.waitingQueuePtr, &taskPtr);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
#if (RK_CONF_MUTEX == ON)
        if (taskPtr->waitingForMutexPtr != NULL)
        {
            kMutexTimeoutWaiter(taskPtr);
        }
#endif
        err = kTCBQEnq(&RK_gReadyQueue[taskPtr->priority], taskPtr);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }

        taskPtr->timeOut = RK_TRUE;
        kSyscallTaskTimeout(taskPtr);
        taskPtr->status = RK_READY;
        kTimeoutNodeReset(&taskPtr->timeoutNode);
        return (err);
    }
    if (taskPtr->timeoutNode.timeoutType == RK_TIMEOUT_TIME_EVENT)
    {
        err = kTCBQEnq(&RK_gReadyQueue[taskPtr->priority], taskPtr);

        K_ASSERT(err == 0);

        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
        /* a time event as any sleep does not change timeOut = TRUE */
        kSyscallTaskTimeout(taskPtr);
        taskPtr->status = RK_READY;
        kTimeoutNodeReset(&taskPtr->timeoutNode);
        return (err);
    }
    if (taskPtr->timeoutNode.timeoutType == RK_TIMEOUT_EVENTFLAGS)
    {
        err = kTCBQEnq(&RK_gReadyQueue[taskPtr->priority], taskPtr);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
        taskPtr->timeOut = RK_TRUE;
        kSyscallTaskTimeout(taskPtr);
        taskPtr->status = RK_READY;
        kTimeoutNodeReset(&taskPtr->timeoutNode);
        return (err);
    }
#if (RK_CONF_SYNCH_MESG == ON)
    if (taskPtr->timeoutNode.timeoutType == RK_TIMEOUT_SYNCH_SEND)
    {
        kSynchMesgTimeoutSend(taskPtr);
        err = kTCBQEnq(&RK_gReadyQueue[taskPtr->priority], taskPtr);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
        taskPtr->timeOut = RK_TRUE;
        kSyscallTaskTimeout(taskPtr);
        taskPtr->status = RK_READY;
        kTimeoutNodeReset(&taskPtr->timeoutNode);
        return (err);
    }
    if (taskPtr->timeoutNode.timeoutType == RK_TIMEOUT_SYNCH_RECV)
    {
        taskPtr->synchMesgRecvBufPtr = NULL;
        taskPtr->synchMesgRecvBytesPtr = NULL;
        taskPtr->synchMesgRecvStatus = RK_ERR_TIMEOUT;
        err = kTCBQEnq(&RK_gReadyQueue[taskPtr->priority], taskPtr);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
        taskPtr->timeOut = RK_TRUE;
        kSyscallTaskTimeout(taskPtr);
        taskPtr->status = RK_READY;
        kTimeoutNodeReset(&taskPtr->timeoutNode);
        return (err);
    }
    if (taskPtr->timeoutNode.timeoutType == RK_TIMEOUT_SYNCH_CALL)
    {
        kSynchMesgTimeoutCall(taskPtr);
        err = kTCBQEnq(&RK_gReadyQueue[taskPtr->priority], taskPtr);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
        taskPtr->timeOut = RK_TRUE;
        kSyscallTaskTimeout(taskPtr);
        taskPtr->status = RK_READY;
        kTimeoutNodeReset(&taskPtr->timeoutNode);
        return (err);
    }
#endif

    return (err);
}

/* runs @ systick */
static volatile RK_TIMEOUT_NODE *nodeg;
UINT kHandleTimeoutList(VOID)
{

    RK_ERR waitingExp = -1;

    if ((RK_gTimeOutListHeadPtr != NULL) && (RK_gTimeOutListHeadPtr->dtick > 0))
    {
        RK_gTimeOutListHeadPtr->dtick--;
    }

    /*  possible to have a node which offset is already (dtick == 0) */
    while (RK_gTimeOutListHeadPtr != NULL && RK_gTimeOutListHeadPtr->dtick == 0)
    {
        nodeg = RK_gTimeOutListHeadPtr;
        waitingExp = kRemoveTimeoutNode((RK_TIMEOUT_NODE *)nodeg);
        K_ASSERT(waitingExp == RK_ERR_SUCCESS);
        if (waitingExp != RK_ERR_SUCCESS)
        {
            return (RK_FALSE);
        }
        waitingExp = kTimeoutNodeReady(nodeg);
    }
    return (waitingExp == RK_ERR_SUCCESS);
}

/*
 * Remove a node from a delta list and donate its remaining delta to the next
 * node, preserving absolute expiration times for every later node.
 */
RK_ERR kRemoveTimeoutNode(RK_TIMEOUT_NODE *node)
{
    if (node == NULL)
        return (RK_ERR_NULL_TIMEOUT_NODE);

    if (kTimeoutNodeIsArmed(node) == RK_FALSE)
    {
        return (RK_ERR_ERROR);
    }

    if (node->nextPtr != NULL)
    {
        node->nextPtr->dtick += node->dtick;
        node->nextPtr->prevPtr = node->prevPtr;
    }

    if (node->prevPtr != NULL)
    {
        node->prevPtr->nextPtr = node->nextPtr;
    }
    else
    {
        *(node->listRefPtr) = node->nextPtr;
    }

    node->nextPtr = NULL;
    node->prevPtr = NULL;
    node->listRefPtr = NULL;
    node->dtick = 0UL;
    return (RK_ERR_SUCCESS);
}
