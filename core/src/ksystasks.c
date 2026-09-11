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
 *   Kernel-owned system tasks. Idle provides the fallback runnable context;
 *   PostProc drains bounded deferred work, including task-fault cleanup, after
 *   handlers leave critical paths.
 */

#define RK_SOURCE_CODE
#include <ksystasks.h>
#include <ksch.h>
#include <kerr.h>
#include <ktaskevents.h>
#include <ksleepq.h>
#include <kmesgq.h>
#include <ktrace.h>

UINT RK_gIdleStack[RK_CONF_IDLE_STACKSIZE] K_ALIGN(8);
UINT RK_gPostProcStack[RK_CONF_POSTPROC_STACKSIZE] K_ALIGN(8);

#define RK_POSTPROC_Q_LEN ((UINT)RK_NTHREADS)

/*
 * Deferred post-processing jobs are bounded work that should not run in an ISR
 * or in a fragile fault handler. The queue is fixed-size; fault cleanup has a
 * separate per-TID pending array so it cannot be lost behind ordinary jobs.
 */
typedef struct RK_POSTPROC_JOB_ENTRY
{
    UINT jobType;
    VOID *objPtr;
    UINT nTasks;
} RK_POSTPROC_JOB_ENTRY;

static RK_POSTPROC_JOB_ENTRY RK_gPostProcQ[RK_POSTPROC_Q_LEN];
static volatile UINT RK_gPostProcHead = 0U;
static volatile UINT RK_gPostProcTail = 0U;
static volatile UINT RK_gPostProcCount = 0U;
static volatile UINT RK_gFaultCleanupPending[RK_NTHREADS];

/*
 * Fault cleanup invariant: one bit per user task is enough. Multiple faults for
 * the same TID coalesce, and cleanup resolves the current registry entry before
 * touching task memory.
 */
static VOID kRunFaultCleanupJobs_(VOID)
{
    for (RK_TID tid = RK_N_SYSTASKS; tid < RK_NTHREADS; tid++)
    {
        RK_CR_AREA
        RK_CR_ENTER
        UINT const pending = RK_gFaultCleanupPending[tid];
        RK_gFaultCleanupPending[tid] = 0U;
        RK_CR_EXIT

        if (pending == 0U)
        {
            continue;
        }

        RK_ERR const err = kTaskFaultCleanup(tid);
        if (err != RK_ERR_SUCCESS)
        {
            K_PANIC("FAULT CLEANUP FAILED");
        }
    }
}

/* Dequeue one ordinary deferred job with head/tail/count updated atomically. */
static RK_ERR kPostProcJobDeq_(RK_POSTPROC_JOB_ENTRY *const jobPtr)
{
    RK_CR_AREA
    RK_CR_ENTER

    if ((jobPtr == NULL) || (RK_gPostProcCount == 0U))
    {
        RK_CR_EXIT
        return (RK_ERR_LIST_EMPTY);
    }

    *jobPtr = RK_gPostProcQ[RK_gPostProcHead];
    RK_gPostProcHead = (RK_gPostProcHead + 1U) % RK_POSTPROC_Q_LEN;
    RK_gPostProcCount -= 1U;

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

/* Drain fault cleanup first, then at most one full queue worth of normal jobs. */
static VOID kRunPostProcJobs_(VOID)
{
    UINT budget = RK_POSTPROC_Q_LEN;

    kRunFaultCleanupJobs_();

    while (budget > 0U)
    {
        RK_POSTPROC_JOB_ENTRY job;
        if (kPostProcJobDeq_(&job) != RK_ERR_SUCCESS)
        {
            break;
        }

        switch (job.jobType)
        {
#if (RK_CONF_SLEEP_QUEUE == ON)
            case RK_POSTPROC_JOB_SLEEPQ_WAKE:
                kSleepQueueWake((RK_SLEEP_QUEUE_HANDLE)(UINTPTR)job.objPtr,
                                job.nTasks, NULL);
                break;
#endif
#if (RK_CONF_MESG_QUEUE == ON)
            case RK_POSTPROC_JOB_MESGQ_RESET:
                kMesgQueueReset((RK_MESG_QUEUE_HANDLE)(UINTPTR)job.objPtr);
                break;
            case RK_POSTPROC_JOB_MESGQ_BROADCAST_WAKE:
                kMesgQueueBroadcastWake((RK_MESG_QUEUE *)job.objPtr,
                                        job.nTasks);
                break;
#endif
#if (RK_CONF_DYNAMIC_TASK == ON)
            case RK_POSTPROC_JOB_TASK_TERMINATE:
                kTaskTerminateTcb((RK_TCB *)job.objPtr);
                break;
#endif
            case RK_POSTPROC_JOB_TASK_FAULT_CLEANUP:
                if (kTaskFaultCleanup((RK_TID)job.nTasks) != RK_ERR_SUCCESS)
                {
                    K_PANIC("FAULT CLEANUP FAILED");
                }
                break;
            default:
                break;
        }
        budget -= 1U;
    }
}

/* Schedule fault cleanup through the non-overflowing per-TID pending array. */
RK_ERR kPostProcFaultCleanupPend(RK_TID const tid)
{
    if ((tid < RK_N_SYSTASKS) || (tid >= RK_NTHREADS))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    RK_CR_AREA
    RK_CR_ENTER
    RK_gFaultCleanupPending[tid] = 1U;
    RK_ERR const err = kEventSet(RK_gPostProcTaskHandle, RK_POSTPROC_SIG);
    RK_CR_EXIT

    return (err);
}

/* Enqueue bounded deferred work; callers must tolerate RK_ERR_NOWAIT on full. */
RK_ERR kPostProcJobEnq(UINT jobType, VOID *const objPtr, UINT nTasks)
{
#if (RK_CONF_ERR_CHECK == ON)
    if (objPtr == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        return (RK_ERR_OBJ_NULL);
    }

    UINT validType = RK_FALSE;
#if (RK_CONF_SLEEP_QUEUE == ON)
    if (jobType == RK_POSTPROC_JOB_SLEEPQ_WAKE)
    {
        validType = RK_TRUE;
    }
#endif
#if (RK_CONF_MESG_QUEUE == ON)
    if (jobType == RK_POSTPROC_JOB_MESGQ_RESET)
    {
        validType = RK_TRUE;
    }
    if (jobType == RK_POSTPROC_JOB_MESGQ_BROADCAST_WAKE)
    {
        validType = RK_TRUE;
    }
#endif
    if (jobType == RK_POSTPROC_JOB_TASK_TERMINATE)
    {
        validType = RK_TRUE;
    }
    if (jobType == RK_POSTPROC_JOB_TASK_FAULT_CLEANUP)
    {
        validType = RK_TRUE;
    }
    if (validType == RK_FALSE)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        return (RK_ERR_INVALID_PARAM);
    }
#endif

    RK_CR_AREA
    RK_CR_ENTER

    if (RK_gPostProcCount >= RK_POSTPROC_Q_LEN)
    {
        RK_CR_EXIT
        return (RK_ERR_NOWAIT);
    }

    RK_gPostProcQ[RK_gPostProcTail].jobType = jobType;
    RK_gPostProcQ[RK_gPostProcTail].objPtr = objPtr;
    RK_gPostProcQ[RK_gPostProcTail].nTasks = nTasks;

    RK_gPostProcTail = (RK_gPostProcTail + 1U) % RK_POSTPROC_Q_LEN;
    RK_gPostProcCount += 1U;

    RK_ERR const err = kEventSet(RK_gPostProcTaskHandle, RK_POSTPROC_SIG);
    RK_CR_EXIT
    return (err);
}

VOID IdleTask(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        RK_ISB

        RK_WFI

        RK_DSB
    }
}

VOID PostProcSysTask(VOID *args)
{
    RK_UNUSEARGS

    RK_REG_SYSTICK_CTRL |= 0x01;

    while (1)
    {
        ULONG gotFlags = 0;

        kEventGet((RK_POSTPROC_SIG | RK_POSTPROC_TIMER_SIG), RK_OPT_EVENT_ANY,
                  &gotFlags, RK_WAIT_FOREVER);

        if ((gotFlags & RK_POSTPROC_SIG) != 0U)
        {
            kRunPostProcJobs_();
        }

#if (RK_CONF_CALLOUT_TIMER == ON)
        if ((gotFlags & RK_POSTPROC_TIMER_SIG) != 0U)
        {
            while (RK_gTimerListHeadPtr != NULL &&
                   RK_gTimerListHeadPtr->dtick == 0)
            {
                RK_TIMEOUT_NODE *node = (RK_TIMEOUT_NODE *)RK_gTimerListHeadPtr;
                RK_gTimerListHeadPtr = node->nextPtr;
                kRemoveTimerNode(node);

                RK_TIMER *timer =
                    K_GET_CONTAINER_ADDR(node, RK_TIMER, timeoutNode);
                kTraceRecordObject(timer, RK_TRACE_OP_EXPIRE,
                                   RK_ERR_SUCCESS, timer->reload);
                if (timer->funPtr != NULL)
                {
                    timer->funPtr(timer->argsPtr);
                }
                if (timer->reload > 0)
                {
                    RK_TICK now = kTickGet();
                    RK_TICK base = timer->nextTime;
                    RK_TICK elapsed = K_TICK_DELTA(now, base);
                    RK_TICK skips = ((elapsed / timer->period) + 1);
                    RK_TICK offset = (RK_TICK)(skips * timer->period);
                    timer->nextTime = K_TICK_ADD(base, offset);
                    RK_TICK delay = K_TICK_DELTA(timer->nextTime, now);
                    if (delay == 0)
                        K_PANIC("0 DELAY TIMER");
                    kTimerReload((RK_TIMER_HANDLE)(UINTPTR)timer, delay);
                }
            }
        }
#endif
    }
}
