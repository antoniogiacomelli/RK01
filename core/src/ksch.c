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
 *   Scheduler and task lifecycle core. It owns task handle generation, task
 *   creation/termination, priority changes, dispatch entry, and deferred
 *   cleanup after contained MPU task faults.
 *
 * Contracts/invariants:
 *   - RK_gRunPtr names exactly one running task after dispatch starts.
 *   - Ready queues own only RK_READY task nodes; the ready bitmap summarizes
 *     non-empty ready queues for priorities that fit in one ULONG.
 *   - Public task handles are encoded TID/generation tokens, not TCB pointers.
 *   - Fault cleanup invalidates registry visibility before freeing or
 *     quarantining task storage.
 */

#define RK_SOURCE_CODE

#include <ksch.h>
#include <kcoredefs.h>
#include <kdynobjs.h>
#include <kmem.h>
#include <kmesg.h>
#include <kmrm.h>
#include <kmutex.h>
#include <ksystasks.h>
#include <ksyscall.h>
#include <ksignal.h>
#include <ksynchmesg.h>
#include <ktimer.h>
#include <ktrace.h>
#if (RK_CONF_FAULT_PRINT_STDERR == ON)
#include <stdio.h>
#endif

/* scheduler globals */
RK_TCBQ RK_gReadyQueue[RK_RDYQSIZ]; /* Table of ready queues */
RK_TCB *RK_gRunPtr;
RK_TCB RK_gTcbs[RK_NTHREADS];
RK_TASK_HANDLE RK_gPostProcTaskHandle;
RK_TASK_HANDLE RK_gIdleTaskHandle;
volatile struct RK_STRUCT_RUNTIME RK_gRunTime;
volatile ULONG RK_gReadyBitmask;
volatile ULONG RK_gReadyPos;
volatile UINT RK_gPendingCtxtSwtch = 0;
volatile UINT RK_gSchLock = 0;
volatile UINT RK_gStartupSvcArmed = 0U;
volatile UINT RK_gKernelPhase = RK_KERNEL_PHASE_BOOT;
volatile UINT RK_gKernelConstructionDepth = 0U;
extern BYTE __rk_app_domain_begin;
extern BYTE __rk_app_domain_end;
/* local globals  */
static RK_TCB const *RK_gKernelConstructionOwnerPtr = NULL;
static volatile UINT RK_gKernelConstructionSchedLocks = 0U;
static RK_PRIO highestPrio = 0;
static RK_PRIO const lowestPrio = RK_CONF_MIN_PRIO;
static volatile RK_PRIO nextTaskPrio = 0;
static RK_PRIO const idleTaskPrio = RK_CONF_MIN_PRIO + 1;
static RK_PRIO const readyBitmaskPrioLimit = sizeof(ULONG) * 8U;
static RK_TID pPid = 0; /* number of active tasks */
static RK_BOOL RK_gTaskPoolInit = RK_FALSE;
static RK_BOOL RK_gSystemTasksInit = RK_FALSE;
static RK_BOOL RK_gApplicationDomainInit = RK_FALSE;
static RK_DOMAIN RK_gApplicationDomain;
static RK_MEM_PARTITION RK_gTaskPool;
static RK_MEM_PARTITION *RK_gTaskDynStackPartByPid[RK_NTHREADS];
static USHORT RK_gTaskHandleGenerationByPid[RK_NTHREADS];
static RK_BOOL RK_gTaskQuarantineByPid[RK_NTHREADS];
RK_TCB *RK_gTaskHandleByPid[RK_NTHREADS];
static CHAR RK_gTaskPublicNameByPid[RK_NTHREADS][RK_OBJ_MAX_NAME_LEN]
    K_ALIGN(4) RK_SECTION_SHARED_BSS;

#define RK_SYSTEM_TASK_CONTROL (RK_CONTROL_PSP_PRIVILEGED)
#define RK_SYSTEM_TASK_PROTECTED (RK_FALSE)

static inline VOID kPendCtxSwtchNow_(VOID)
{
    RK_gPendingCtxtSwtch = 0U;
    RK_DSB
    RK_PEND_CTXTSWTCH
    RK_ISB
}

static inline VOID kDeferCtxSwtch_(VOID)
{
    RK_gPendingCtxtSwtch = 1U;
    RK_BARRIER
}

static inline RK_PRIO kCalcNextTaskPrio_(VOID);
static RK_ERR kTaskEnsureSystemTasks_(VOID);
static UINT kTickHandlerRun_(VOID);
static RK_BOOL kTaskPointerInPool_(RK_TCB const *const taskPtr);

RK_BOOL kKernelBootPhase(VOID)
{
    return ((RK_gKernelPhase == RK_KERNEL_PHASE_BOOT) ? RK_TRUE : RK_FALSE);
}

RK_BOOL kKernelRunning(VOID)
{
    return ((RK_gKernelPhase == RK_KERNEL_PHASE_RUNNING) ? RK_TRUE : RK_FALSE);
}

VOID kKernelConstructionEnter(VOID)
{
    if ((RK_gRunPtr != NULL) && (RK_gRunPtr->preempt != RK_NO_PREEMPT))
    {
        kSchLock();
        RK_gKernelConstructionSchedLocks++;
    }

    if (RK_gKernelConstructionDepth == 0U)
    {
        RK_gKernelConstructionOwnerPtr = RK_gRunPtr;
    }

    RK_gKernelConstructionDepth++;
    RK_BARRIER
}

VOID kKernelConstructionExit(VOID)
{
    RK_BOOL unlock = RK_FALSE;

    if (RK_gKernelConstructionDepth > 0U)
    {
        if (RK_gKernelConstructionSchedLocks > 0U)
        {
            RK_gKernelConstructionSchedLocks--;
            unlock = RK_TRUE;
        }

        RK_gKernelConstructionDepth--;
        if (RK_gKernelConstructionDepth == 0U)
        {
            RK_gKernelConstructionOwnerPtr = NULL;
        }
    }
    RK_BARRIER

    if (unlock == RK_TRUE)
    {
        kSchUnlock();
    }
}

RK_BOOL kKernelRawInitAllowed(VOID)
{
    if (RK_gKernelPhase == RK_KERNEL_PHASE_BOOT)
    {
        return (RK_TRUE);
    }

    if (RK_gKernelConstructionDepth == 0U)
    {
        return (RK_FALSE);
    }

    return ((RK_gKernelConstructionOwnerPtr == RK_gRunPtr) ? RK_TRUE
                                                           : RK_FALSE);
}

RK_ERR kKernelRawInitGuard(VOID)
{
    if (kKernelRawInitAllowed() == RK_TRUE)
    {
        return (RK_ERR_SUCCESS);
    }

#if (RK_CONF_ERR_CHECK == ON)
    kErrHandler(RK_FAULT_INVALID_PHASE);
#endif
    return (RK_ERR_INVALID_PHASE);
}

static RK_TID kTaskFindFreeTid_(VOID)
{
    for (RK_TID tid = RK_N_SYSTASKS; tid < RK_NTHREADS; tid++)
    {
        if ((RK_gTaskHandleByPid[tid] == NULL) &&
            (RK_gTaskQuarantineByPid[tid] == RK_FALSE))
        {
            return (tid);
        }
    }

    return ((RK_TID)RK_NTHREADS);
}

static RK_ERR kTaskRegister_(RK_TCB *const taskPtr, RK_TID const tid)
{
    if (taskPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if ((tid >= RK_NTHREADS) || (RK_gTaskHandleByPid[tid] != NULL) ||
        (RK_gTaskQuarantineByPid[tid] == RK_TRUE))
    {
        return (RK_ERR_TASK_POOL_EMPTY);
    }

    RK_gTaskHandleByPid[tid] = taskPtr;
    RK_gTaskDynStackPartByPid[tid] = NULL;
    return (RK_ERR_SUCCESS);
}

/*
 * Task handles use the same slot/generation idea as runtime objects. Bumping
 * on invalidation makes any cached handle for this TID fail after the TCB slot
 * is reused.
 */
static VOID kTaskHandleGenerationBump_(RK_TID const tid)
{
    if (tid < RK_NTHREADS)
    {
        RK_gTaskHandleGenerationByPid[tid]++;
    }
}

/*
 * Remove the task from the handle registry before cleanup can release or reuse
 * its TCB memory. The generation bump is the stale-handle barrier.
 */
static VOID kTaskRegistryInvalidate_(RK_TCB const *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return;
    }

    RK_TID const tid = taskPtr->tid;
    if ((tid < RK_NTHREADS) && (RK_gTaskHandleByPid[tid] == taskPtr))
    {
        RK_gTaskHandleByPid[tid] = NULL;
        kTaskHandleGenerationBump_(tid);
    }
}

/*
 * Validate the public task-token shape and extract its registry coordinates.
 * Liveness is checked later against RK_gTaskHandleByPid[] and the generation
 * table, so this helper remains purely about the bit-field contract.
 */
static RK_BOOL kTaskHandleDecode_(RK_TASK_HANDLE const taskHandle,
                                  RK_TID *const tidPtr,
                                  USHORT *const generationPtr)
{
    ULONG const rawHandle = (ULONG)(UINTPTR)taskHandle;

    if (((rawHandle & RK_HANDLE_TAG_MASK) != RK_HANDLE_TAG) ||
        ((rawHandle & RK_HANDLE_TYPE_MASK) != RK_HANDLE_TYPE_TASK))
    {
        return (RK_FALSE);
    }

    RK_TID const tid = (RK_TID)(rawHandle & RK_HANDLE_SLOT_MASK);
    if (tid >= RK_NTHREADS)
    {
        return (RK_FALSE);
    }

    if (tidPtr != NULL)
    {
        *tidPtr = tid;
    }
    if (generationPtr != NULL)
    {
        *generationPtr =
            (USHORT)((rawHandle >> RK_HANDLE_GENERATION_SHIFT) &
                     RK_HANDLE_GENERATION_MASK);
    }

    return (RK_TRUE);
}

/*
 * Publish the current registry coordinates as a task handle. The pointer-sized
 * value deliberately does not point at the TCB, so unprivileged code cannot
 * dereference kernel task memory.
 */
RK_TASK_HANDLE kTaskHandleFromTcb(RK_TCB const *const taskPtr)
{
    if ((taskPtr == NULL) || (taskPtr->tid >= RK_NTHREADS) ||
        (RK_gTaskHandleByPid[taskPtr->tid] != taskPtr))
    {
        return (NULL);
    }

    ULONG const generation =
        ((ULONG)RK_gTaskHandleGenerationByPid[taskPtr->tid]) &
        RK_HANDLE_GENERATION_MASK;
    ULONG const rawHandle = RK_HANDLE_TAG | RK_HANDLE_TYPE_TASK |
                            (generation << RK_HANDLE_GENERATION_SHIFT) |
                            ((ULONG)taskPtr->tid & RK_HANDLE_SLOT_MASK);

    return ((RK_TASK_HANDLE)(UINTPTR)rawHandle);
}

RK_ERR kTaskHandleResolve(RK_TASK_HANDLE const taskHandle,
                          RK_TCB **const taskPPtr)
{
    RK_TID tid = 0U;
    USHORT generation = 0U;

    if (taskPPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    *taskPPtr = NULL;

    if (taskHandle == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (kTaskHandleDecode_(taskHandle, &tid, &generation) == RK_FALSE)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if (generation != RK_gTaskHandleGenerationByPid[tid])
    {
        return (RK_ERR_INVALID_OBJ);
    }

    RK_TCB *const taskPtr = RK_gTaskHandleByPid[tid];
    if ((taskPtr == NULL) || (taskPtr->tid != tid) ||
        (kTaskPointerInPool_(taskPtr) == RK_FALSE))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if (taskPtr->init != RK_TRUE)
    {
        return (RK_ERR_OBJ_NOT_INIT);
    }

    *taskPPtr = taskPtr;
    return (RK_ERR_SUCCESS);
}

RK_ERR kTaskHandleResolveOrRunning(RK_TASK_HANDLE const taskHandle,
                                   RK_TCB **const taskPPtr)
{
    if (taskPPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (taskHandle != NULL)
    {
        return (kTaskHandleResolve(taskHandle, taskPPtr));
    }

    if ((RK_gRunPtr == NULL) || (RK_gRunPtr->init != RK_TRUE))
    {
        *taskPPtr = NULL;
        return (RK_ERR_INVALID_OBJ);
    }

    *taskPPtr = RK_gRunPtr;
    return (RK_ERR_SUCCESS);
}

static RK_BOOL kTaskPointerInPool_(RK_TCB const *const taskPtr)
{
    UINTPTR const taskAddr = (UINTPTR)taskPtr;
    UINTPTR const poolBegin = (UINTPTR)&RK_gTcbs[0];
    UINTPTR const poolEnd = (UINTPTR)&RK_gTcbs[RK_NTHREADS];

    if ((taskAddr < poolBegin) || (taskAddr >= poolEnd))
    {
        return (RK_FALSE);
    }

    return ((((taskAddr - poolBegin) % (UINTPTR)sizeof(RK_TCB)) == 0UL) ?
                RK_TRUE : RK_FALSE);
}

static RK_ERR kTaskPoolReserveBlock_(RK_TCB *const taskPtr)
{
    BYTE *prevPtr = NULL;
    BYTE *currPtr;

    if (kTaskPointerInPool_(taskPtr) == RK_FALSE)
    {
        return (RK_ERR_SUCCESS);
    }

    if (RK_gTaskPoolInit == RK_FALSE)
    {
        return (RK_ERR_TASK_POOL_NOT_INIT);
    }

    currPtr = RK_gTaskPool.freeListPtr;

    for (ULONG idx = 0UL; (idx < RK_gTaskPool.nFreeBlocks) &&
                         (currPtr != NULL); idx++)
    {
        BYTE *const nextPtr = *(BYTE **)currPtr;

        if ((RK_TCB *)currPtr == taskPtr)
        {
            if (prevPtr == NULL)
            {
                RK_gTaskPool.freeListPtr = nextPtr;
            }
            else
            {
                *(BYTE **)prevPtr = nextPtr;
            }

            RK_gTaskPool.nFreeBlocks--;
            kTraceRecordObject(&RK_gTaskPool, RK_TRACE_OP_ALLOC,
                               RK_ERR_SUCCESS, RK_gTaskPool.nFreeBlocks);
            return (RK_ERR_SUCCESS);
        }

        prevPtr = currPtr;
        currPtr = nextPtr;
    }

    return (RK_ERR_INVALID_OBJ);
}

/* compile-time assertions trick */
#ifndef RK_DISABLE_STATIC_ASSERTS
typedef char RK_TCB_SP_OFFSET_ASSERT[(offsetof(RK_TCB, sp) == 0U) ? 1 : -1];
typedef char
    RK_TCB_STATUS_OFFSET_ASSERT[(offsetof(RK_TCB, status) == 4U) ? 1 : -1];
typedef char
    RK_TCB_RUNCNT_OFFSET_ASSERT[(offsetof(RK_TCB, runCnt) == 8U) ? 1 : -1];
typedef char
    RK_TCB_SAVEDLR_OFFSET_ASSERT[(offsetof(RK_TCB, savedLR) == 12U) ? 1 : -1];
typedef char
    RK_TCB_STACKADDR_OFFSET_ASSERT[(offsetof(RK_TCB, stackBufPtr) == 16U) ? 1
                                                                          : -1];
typedef char
ASSERT_ADDR_SIZEOF_ULONG[(sizeof(RK_ADDR) == sizeof(UINTPTR)) ? 1 : -1];
typedef char
    RK_READY_QUEUE_LOWEST_PRIO_ASSERT[(RK_CONF_MIN_PRIO < RK_RDYQSIZ) ? 1 : -1];
typedef char
    RK_READY_QUEUE_IDLE_PRIO_ASSERT[((RK_CONF_MIN_PRIO + 1U) < RK_RDYQSIZ) ? 1 : -1];

#endif


/******************************************************************************/
/* SCHEDULER LOCK                                                             */
/******************************************************************************/
/* shall be called while irqs are disabled */
VOID kPendCtxSwtch(VOID)
{
    if ((RK_gRunPtr != NULL) && (RK_gRunPtr->status == RK_READY) &&
        (RK_gSchLock > 0U))
    {
        kDeferCtxSwtch_();
    }
    else if ((RK_gRunPtr != NULL) && (RK_gRunPtr->status != RK_RUNNING))
    {
        kPendCtxSwtchNow_();
    }
    else if (RK_gSchLock == 0U)
    {
        kPendCtxSwtchNow_();
    }
    else
    {
        kDeferCtxSwtch_();
    }
}

VOID kSchLock(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        kSyscallInvoke4(RK_SYSCALL_SCH_LOCK, 0UL, 0UL, 0UL, 0UL);
        return;
    }

    if (RK_gRunPtr->preempt == 0UL)
    {
        return;
    }
    RK_CR_AREA
    RK_CR_ENTER
    RK_gSchLock++;
    RK_gRunPtr->schLock = RK_gSchLock;
    RK_DSB
    RK_ISB
    RK_CR_EXIT
}

VOID kSchUnlock(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        kSyscallInvoke4(RK_SYSCALL_SCH_UNLOCK, 0UL, 0UL, 0UL, 0UL);
        return;
    }

    if (RK_gSchLock == 0UL)
    {
        return;
    }
    RK_CR_AREA
    RK_CR_ENTER
    RK_gSchLock--;
    RK_gRunPtr->schLock = RK_gSchLock;
    if ((RK_gSchLock == 0U) && (RK_gPendingCtxtSwtch != 0U))
    {
        kPendCtxSwtchNow_();
    }
    RK_CR_EXIT
}

/******************************************************************************/
/* TASK QUEUE MANAGEMENT                                                      */
/******************************************************************************/
/*
 * The ready bitmap is a fast summary of non-empty ready queues for priorities
 * that fit in one ULONG. If a port configures more priorities than that, the
 * list queues remain authoritative and the bitmap simply does not track the
 * out-of-range priorities.
 */
static inline RK_BOOL kReadyBitmaskTracksPrio_(RK_PRIO prio)
{
    return ((RK_BOOL)(prio < readyBitmaskPrioLimit));
}

/* Mark a priority as runnable in the summary bitmap. */
static inline VOID kReadyBitmaskSet_(RK_PRIO prio)
{
    if (kReadyBitmaskTracksPrio_(prio) == RK_TRUE)
    {
        RK_gReadyBitmask |= 1UL << prio;
    }
}

/* Clear a priority from the summary bitmap after its ready queue becomes empty. */
static inline VOID kReadyBitmaskClear_(RK_PRIO prio)
{
    if (kReadyBitmaskTracksPrio_(prio) == RK_TRUE)
    {
        RK_gReadyBitmask &= ~(1UL << prio);
    }
}

RK_ERR kTCBQInit(RK_TCBQ *const kobj)
{

    RK_ERR err = kListInit(kobj);

    return (err);
}

RK_ERR kTCBQEnq(RK_TCBQ *const kobj, RK_TCB *const tcbPtr)
{

    RK_ERR err = kListAddTail(kobj, &(tcbPtr->tcbNode));
    if (kobj == &RK_gReadyQueue[tcbPtr->priority])
    {
        kReadyBitmaskSet_(tcbPtr->priority);
    }

    return (err);
}

RK_ERR kTCBQJam(RK_TCBQ *const kobj, RK_TCB *const tcbPtr)
{

    RK_ERR err = kListAddHead(kobj, &(tcbPtr->tcbNode));
    if (kobj == &RK_gReadyQueue[tcbPtr->priority])
    {
        kReadyBitmaskSet_(tcbPtr->priority);
        RK_DMB
    }
    return (err);
}

RK_ERR kTCBQDeq(RK_TCBQ *const kobj, RK_TCB **const tcbPPtr)
{
    RK_NODE *dequeuedNodePtr = NULL;
    RK_ERR err = kListRemoveHead(kobj, &dequeuedNodePtr);
    *tcbPPtr = K_GET_TCB_ADDR(dequeuedNodePtr);
    K_ASSERT(*tcbPPtr != NULL);
    RK_TCB const *tcbPtr_ = *tcbPPtr;
    RK_PRIO prio_ = tcbPtr_->priority;
    if ((kobj == &RK_gReadyQueue[prio_]) && (kobj->size == 0))
    {
        kReadyBitmaskClear_(prio_);
        RK_DMB
    }
    return (err);
}

RK_ERR kTCBQRem(RK_TCBQ *const kobj, RK_TCB **const tcbPPtr)
{
    RK_NODE *dequeuedNodePtr = &((*tcbPPtr)->tcbNode);
    kListRemove(kobj, dequeuedNodePtr);
    *tcbPPtr = K_GET_TCB_ADDR(dequeuedNodePtr);
    RK_TCB const *tcbPtr_ = *tcbPPtr;
    RK_PRIO prio_ = tcbPtr_->priority;
    if ((kobj == &RK_gReadyQueue[prio_]) && (kobj->size == 0))
    {
        kReadyBitmaskClear_(prio_);
        RK_DMB
    }
    return (RK_ERR_SUCCESS);
}

RK_TCB *kTCBQPeek(RK_TCBQ *const kobj)
{
    RK_NODE *nodePtr = kobj->listDummy.nextPtr;
    RK_TCB *retPtr = (K_GET_CONTAINER_ADDR(nodePtr, RK_TCB, tcbNode));
    return (retPtr);
}

RK_ERR kTCBQEnqByPrio(RK_TCBQ *const kobj, RK_TCB *const tcbPtr)
{
    RK_NODE *currNodePtr = &(kobj->listDummy);

    while (currNodePtr->nextPtr != &(kobj->listDummy))
    {
        RK_TCB const *currTcbPtr = K_GET_TCB_ADDR(currNodePtr->nextPtr);
        if (currTcbPtr->priority > tcbPtr->priority)
        {
            break;
        }
        currNodePtr = currNodePtr->nextPtr;
    }

    RK_ERR err = kListInsertAfter(kobj, currNodePtr, &(tcbPtr->tcbNode));

    return (err);
}

/* reeschedule a task based on current running priority, preemptibility and
scheduler lock state */
RK_ERR kReschedTask(RK_TCB *tcbPtr)
{

    if ((RK_gRunPtr->priority > tcbPtr->priority) && RK_gRunPtr->preempt == 1UL)
    {
        if (RK_gSchLock == 0UL)
        {
            kPendCtxSwtchNow_();
            return (RK_ERR_SUCCESS); /* RUNNING prio is lower*/
        }
        else
        {
            kDeferCtxSwtch_();
            return (RK_ERR_RESCHED_PENDING); /* RUNNING prio is lower but
                                                scheduler is locked */
        }
    }
    return (RK_ERR_RESCHED_NOT_NEEDED); /* RUNNING prio is higher */
}

/* Re-evaluate dispatch after the RUNNING task priority changes in place. */
RK_ERR kReschedRunning(VOID)
{
    RK_PRIO const readyPrio = kCalcNextTaskPrio_();

    if ((RK_gRunPtr != NULL) && (RK_gRunPtr->status == RK_RUNNING) &&
        (RK_gRunPtr->preempt == 1UL) && (readyPrio < RK_gRunPtr->priority))
    {
        kPendCtxSwtch();
        if (RK_gSchLock != 0UL)
        {
            return (RK_ERR_RESCHED_PENDING);
        }
        return (RK_ERR_SUCCESS);
    }
    return (RK_ERR_RESCHED_NOT_NEEDED);
}

static inline RK_PRIO kTaskMinPrio_(RK_PRIO const currentPrio,
                                    RK_PRIO const candidatePrio)
{
    return ((candidatePrio < currentPrio) ? candidatePrio : currentPrio);
}

#if (RK_CONF_MUTEX == ON)
static RK_PRIO kTaskOwnedMutexPipPrio_(RK_TCB *const ownerTcb,
                                       RK_PRIO const currentPrio)
{
    RK_PRIO newPrio = currentPrio;
    RK_NODE *nodePtr = ownerTcb->ownedMutexList.listDummy.nextPtr;

    while (nodePtr != &ownerTcb->ownedMutexList.listDummy)
    {
        RK_MUTEX *mtxPtr = K_GET_CONTAINER_ADDR(nodePtr, RK_MUTEX, mutexNode);

        if ((mtxPtr->protocol == RK_PRIO_INHERITANCE) &&
            (mtxPtr->waitingQueue.size > 0UL))
        {
            RK_TCB *waiterPtr = kTCBQPeek(&mtxPtr->waitingQueue);
            if (waiterPtr != NULL)
            {
                newPrio = kTaskMinPrio_(newPrio, waiterPtr->priority);
            }
        }

        nodePtr = nodePtr->nextPtr;
        RK_BARRIER
    }

    return (newPrio);
}
#endif

#if (RK_CONF_SYNCH_MESG == ON)
/*
 * Synchronous rendezvous priority contracts.
 *
 * Plain kSynchSendWait()/kSyncRecv() is a blocking copy rendezvous. If the
 * sender has to queue because the receiver is not already waiting, the queued
 * sender can raise the receiver's effective priority until the payload is
 * copied, times out, or cleanup clears the send.
 *
 * kSynchMesgCall()/kSynchMesgAccept()/kSynchMesgReply() is the extended
 * rendezvous. Accept snapshots the caller's effective priority in
 * synchMesgActiveCallerPrio. The server uses that caller priority as the
 * scheduling base until reply, timeout, or cleanup clears the active call.
 * Because this is substitution rather than "min(nominal, caller)", the task can
 * become less urgent than its nominal priority while handling less-urgent call
 * work. Lower numeric RK_PRIO values are more urgent.
 */
static RK_PRIO kTaskSynchMesgBasePrio_(RK_TCB *const taskPtr,
                                       RK_PRIO const currentPrio)
{
    RK_TCB const *const activeCallerPtr = taskPtr->synchMesgActiveCallerPtr;

    if ((activeCallerPtr != NULL) &&
        (activeCallerPtr->synchMesgCallState == RK_SYNCH_CALL_ACTIVE))
    {
        return (taskPtr->synchMesgActiveCallerPrio);
    }

    return (currentPrio);
}

static RK_PRIO kTaskSynchMesgWaiterPrio_(RK_TCB *const taskPtr,
                                         RK_PRIO const currentPrio)
{
    RK_PRIO newPrio = currentPrio;

    if (taskPtr->synchMesgSenders.size > 0UL)
    {
        RK_TCB *senderPtr = kTCBQPeek(&taskPtr->synchMesgSenders);
        if (senderPtr != NULL)
        {
            newPrio = kTaskMinPrio_(newPrio, senderPtr->priority);
        }
    }

    if (taskPtr->synchMesgCallers.size > 0UL)
    {
        RK_TCB *callerPtr = kTCBQPeek(&taskPtr->synchMesgCallers);
        if (callerPtr != NULL)
        {
            newPrio = kTaskMinPrio_(newPrio, callerPtr->priority);
        }
    }

    return (newPrio);
}
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
/*
 * Apply the asynchronous-message priority ceiling. Each owned message points
 * back to its pool, and each pool may contribute one ceiling. Lower numeric
 * RK_PRIO values are higher scheduler priorities, so kTaskMinPrio_() selects
 * the highest effective priority required by all owned message pools.
 */
static RK_PRIO kTaskAsynchMesgCeilingPrio_(RK_TCB *const taskPtr,
                                           RK_PRIO const currentPrio)
{
    RK_PRIO newPrio = currentPrio;
    RK_NODE *nodePtr = taskPtr->asynchMesgOwnedList.listDummy.nextPtr;

    while (nodePtr != &taskPtr->asynchMesgOwnedList.listDummy)
    {
        RK_MESG const *const mesgPtr =
            K_GET_CONTAINER_ADDR(nodePtr, RK_MESG, ownerNode);
        RK_MEM_PARTITION const *const poolPtr = mesgPtr->poolPtr;

        if ((poolPtr != NULL) &&
            (poolPtr->mesgPrioCeilingEnabled == RK_TRUE))
        {
            newPrio = kTaskMinPrio_(newPrio, poolPtr->mesgPrioCeiling);
        }

        nodePtr = nodePtr->nextPtr;
        RK_BARRIER
    }

    return (newPrio);
}
#endif

/*
 * Effective priority is the highest scheduling priority required by every
 * active protocol affecting this task.
 * E.g.: 1) asynch ceiling raises Task A
 *       2) Task A is blocked on mutex owned by Task B
 *       3) Task B may inherit Task A's raised priority
 * This helper only calculates the value; kTaskUpdateEffectivePrio() applies it
 * and requeues/reschedules the task when it changes.
 */
static RK_PRIO kTaskCalcEffectivePrio_(RK_TCB *const taskPtr)
{
    RK_PRIO newPrio = taskPtr->prioNominal;

#if (RK_CONF_SYNCH_MESG == ON)
    /*
     * During an active extended rendezvous, the server adopts the caller's
     * priority as its scheduling base. This can raise or lower the server.
     */
    newPrio = kTaskSynchMesgBasePrio_(taskPtr, newPrio);
#endif

#if (RK_CONF_MUTEX == ON)
    /* Mutex priority inheritance can raise an owner to its highest waiter. */
    newPrio = kTaskOwnedMutexPipPrio_(taskPtr, newPrio);
#endif

#if (RK_CONF_SYNCH_MESG == ON)
    /* Queued synchronous-message senders/callers can impose higher urgency. */
    newPrio = kTaskSynchMesgWaiterPrio_(taskPtr, newPrio);
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
    /*
     * Message ceilings are ownership-based: kMesgSetOwner_() maintains the
     * owned-message list, and this hook folds those ceilings into scheduling.
     */
    newPrio = kTaskAsynchMesgCeilingPrio_(taskPtr, newPrio);
#endif

    return (newPrio);
}

static VOID kTaskRequeueWaiterByPrio_(RK_TCB *const tcbPtr)
{
    RK_LIST *const waitQueuePtr = tcbPtr->timeoutNode.waitingQueuePtr;

    if ((waitQueuePtr == NULL) || (waitQueuePtr->size <= 1UL) ||
        (tcbPtr->tcbNode.nextPtr == NULL) ||
        (tcbPtr->tcbNode.prevPtr == NULL))
    {
        return;
    }

    RK_TCB *requeuePtr = tcbPtr;
    RK_ERR err = kTCBQRem(waitQueuePtr, &requeuePtr);
    K_ASSERT(err == RK_ERR_SUCCESS);

    err = kTCBQEnqByPrio(waitQueuePtr, requeuePtr);
    K_ASSERT(err == RK_ERR_SUCCESS);
}

RK_BOOL kTaskUpdateEffectivePrio(RK_TCB *const tcbPtr)
{
    if ((tcbPtr == NULL) || (tcbPtr->init != RK_TRUE))
    {
        return (RK_FALSE);
    }

    RK_PRIO const newPrio = kTaskCalcEffectivePrio_(tcbPtr);
    if (tcbPtr->priority == newPrio)
    {
        return (RK_FALSE);
    }

    RK_PRIO const oldPrio = tcbPtr->priority;

    if (tcbPtr->status == RK_READY)
    {
        RK_TCB *remPtr = tcbPtr;
        RK_ERR err = kTCBQRem(&RK_gReadyQueue[oldPrio], &remPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);

        tcbPtr->priority = newPrio;
        kTraceRecordTaskPrio(kTaskHandleFromTcb(tcbPtr), oldPrio, newPrio);

        err = kTCBQEnq(&RK_gReadyQueue[tcbPtr->priority], tcbPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);

        if (RK_gRunPtr != NULL)
        {
            kReschedTask(tcbPtr);
        }
    }
    else
    {
        tcbPtr->priority = newPrio;
        kTraceRecordTaskPrio(kTaskHandleFromTcb(tcbPtr), oldPrio, newPrio);
        if ((tcbPtr->status == RK_RUNNING) && (RK_gRunPtr != NULL))
        {
            kReschedRunning();
        }
    }

    return (RK_TRUE);
}

VOID kTaskUpdateEffectivePrioChain(RK_TCB *const tcbPtr)
{
    RK_TCB *currTcbPtr = tcbPtr;

    while (currTcbPtr != NULL)
    {
        RK_BOOL const changed = kTaskUpdateEffectivePrio(currTcbPtr);
        if (changed == RK_FALSE)
        {
            break;
        }

        kTaskRequeueWaiterByPrio_(currTcbPtr);

#if (RK_CONF_MUTEX == ON)
        /*
         * Cross-task propagation only follows mutex PI wait chains. If this
         * task's effective priority changed while it is blocked on an inherited
         * mutex, the mutex owner may need to inherit the new value too.
         */
        if ((currTcbPtr->status != RK_BLOCKED) ||
            (currTcbPtr->waitingForMutexPtr == NULL))
        {
            break;
        }

        RK_MUTEX *const waitMtxPtr = currTcbPtr->waitingForMutexPtr;
        if ((waitMtxPtr->protocol != RK_PRIO_INHERITANCE) ||
            (waitMtxPtr->ownerPtr == NULL))
        {
            break;
        }

        currTcbPtr = waitMtxPtr->ownerPtr;
#else
        break;
#endif
    }

    RK_ISB
}

RK_ERR kReadySwtch(RK_TCB *const tcbPtr)
{
    RK_ERR err = -1;
    if (tcbPtr->tid == RK_POSTPROC_TASK_ID)
    {
        err = kTCBQJam(&RK_gReadyQueue[tcbPtr->priority], tcbPtr);
    }
    else
    {
        err = kTCBQEnq(&RK_gReadyQueue[tcbPtr->priority], tcbPtr);
    }
    if (err == RK_ERR_SUCCESS)
    {
        kSyscallTaskWake(tcbPtr);
        tcbPtr->status = RK_READY;
        return (kReschedTask(tcbPtr));
    }
    return (err);
}
/* ready a task without testing if switch is needed */
RK_ERR kReadyNoSwtch(RK_TCB *const tcbPtr)
{
    K_ASSERT(tcbPtr != NULL);
    RK_ERR err = -1;
    if (tcbPtr->tid == RK_POSTPROC_TASK_ID)
    {
        err = kTCBQJam(&RK_gReadyQueue[tcbPtr->priority], tcbPtr);
    }
    else
    {
        err = kTCBQEnq(&RK_gReadyQueue[tcbPtr->priority], tcbPtr);
    }

    K_ASSERT(err == RK_ERR_SUCCESS);

    kSyscallTaskWake(tcbPtr);
    tcbPtr->status = RK_READY;

    RK_DMB

    return (RK_ERR_SUCCESS);
}

/* fwded private helpers */
static inline VOID kPreemptRunningTask_(VOID);
static inline VOID kYieldRunningTask_(VOID);
static inline RK_PRIO kCalcNextTaskPrio_();

/******************************************************************************/
/* YIELD/CTXT SWTCH RUNNING TASK                                              */
/******************************************************************************/
VOID kYield(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        kSyscallInvoke4(RK_SYSCALL_YIELD, 0UL, 0UL, 0UL, 0UL);
        return;
    }

    RK_CR_AREA
    RK_CR_ENTER
    kYieldRunningTask_();
    RK_CR_EXIT
}

/******************************************************************************/
/* TASK CONTROL BLOCK MANAGEMENT                                              */
/******************************************************************************/
static inline VOID kWriteName_(RK_STRING dstPtr, CHAR const *const name)
{
    UINT idx;

    for (idx = 0U; idx < (RK_OBJ_MAX_NAME_LEN - 1U); idx++)
    {
        dstPtr[idx] = name[idx];
        if (name[idx] == '\0')
        {
            return;
        }
    }

    dstPtr[RK_OBJ_MAX_NAME_LEN - 1U] = '\0';
}

RK_DOMAIN *kApplicationDomainGet(VOID)
{
    return ((RK_gApplicationDomainInit == RK_TRUE) ? &RK_gApplicationDomain
                                                   : NULL);
}

RK_ERR kApplicationDomainEnsureInit(VOID)
{
    if (RK_gApplicationDomainInit == RK_TRUE)
    {
        return (RK_ERR_SUCCESS);
    }

    if (kMpuLayoutIsFinalized() == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PHASE);
#endif
        return (RK_ERR_INVALID_PHASE);
    }

    UINTPTR const regionBegin = (UINTPTR)&__rk_app_domain_begin;
    UINTPTR const regionEnd = (UINTPTR)&__rk_app_domain_end;
    if ((regionEnd <= regionBegin) ||
        ((regionEnd - regionBegin) > (UINTPTR)RK_ULONG_MAX))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }

    RK_MEMSET(&RK_gApplicationDomain, 0, sizeof(RK_gApplicationDomain));
    kWriteName_(RK_gApplicationDomain.domainName, "App");
    RK_gApplicationDomain.regionBasePtr = &__rk_app_domain_begin;
    RK_gApplicationDomain.regionBytes = (ULONG)(regionEnd - regionBegin);
    RK_gApplicationDomain.init = RK_FALSE;

    RK_ERR const err = kMpuDomainMemoryReserve(&RK_gApplicationDomain);
    if (err != RK_ERR_SUCCESS)
    {
        RK_MEMSET(&RK_gApplicationDomain, 0, sizeof(RK_gApplicationDomain));
        return (err);
    }

    RK_gApplicationDomain.init = RK_TRUE;
    RK_gApplicationDomainInit = RK_TRUE;
    return (RK_ERR_SUCCESS);
}

static inline VOID kWriteTaskName_(RK_TCB *const tcbPtr, CHAR * const  name)
{
    kWriteName_(tcbPtr->taskName, name);
}

static CHAR const *kTaskPublicNameSnapshot_(RK_TCB const *const taskPtr)
{
    if ((taskPtr == NULL) || (taskPtr->init != RK_TRUE) ||
        (taskPtr->tid >= RK_NTHREADS))
    {
        return ("");
    }

    RK_STRING dstPtr = &RK_gTaskPublicNameByPid[taskPtr->tid][0];
    for (UINT i = 0U; i < (RK_OBJ_MAX_NAME_LEN - 1U); i++)
    {
        dstPtr[i] = taskPtr->taskName[i];
        if (taskPtr->taskName[i] == '\0')
        {
            return (dstPtr);
        }
    }

    dstPtr[RK_OBJ_MAX_NAME_LEN - 1U] = '\0';
    return (dstPtr);
}

static inline RK_ERR kTaskPoolInit_(ULONG const nTcbs);

static inline RK_ERR kTaskPoolEnsureInit_(ULONG const nTcbs)
{
    if (RK_gTaskPoolInit == RK_TRUE)
    {
        return (RK_ERR_SUCCESS);
    }
    return (kTaskPoolInit_(nTcbs));
}

static inline RK_BOOL kTaskStackGeometryValid_(RK_STACK const *const stackBufPtr,
                                               ULONG const stackSize)
{
    if (stackBufPtr == NULL)
    {
        return (RK_FALSE);
    }

    if ((stackSize < RK_CONF_MIN_STACKSIZE) || ((stackSize & 1UL) != 0UL) ||
        (stackSize > (RK_ULONG_MAX / (ULONG)sizeof(RK_STACK))))
    {
        return (RK_FALSE);
    }
    /* is its address  aligned to 8? */
    return ((((ULONG)stackBufPtr & 0x7UL) == 0UL) ? RK_TRUE : RK_FALSE);
}

static VOID kTaskStackGeometryFault_(RK_STRING const taskName,
                                     RK_STACK const *const stackBufPtr,
                                     ULONG const stackSize)
{
#if (RK_CONF_FAULT_PRINT_STDERR == ON)
    CHAR const *reasonPtr = "valid";

    if (stackBufPtr == NULL)
    {
        reasonPtr = "null stack pointer";
    }
    else if (stackSize < RK_CONF_MIN_STACKSIZE)
    {
        reasonPtr = "stack size below RK_CONF_MIN_STACKSIZE";
    }
    else if ((stackSize & 1UL) != 0UL)
    {
        reasonPtr = "stack size is not double-word aligned";
    }
    else if (stackSize > (RK_ULONG_MAX / (ULONG)sizeof(RK_STACK)))
    {
        reasonPtr = "stack size overflows byte count";
    }
    else if (((ULONG)stackBufPtr & 0x7UL) != 0UL)
    {
        reasonPtr = "stack address is not 8-byte aligned";
    }

    printf("TASK INIT INVALID STACK: task=%s stack=0x%08lx words=%lu "
           "minWords=%lu reason=%s\r\n",
           (taskName != NULL) ? taskName : "(null)",
           (ULONG)stackBufPtr,
           stackSize,
           (ULONG)RK_CONF_MIN_STACKSIZE,
           reasonPtr);
#else
    K_UNUSE(taskName);
    K_UNUSE(stackBufPtr);
    K_UNUSE(stackSize);
#endif

#if (RK_CONF_ERR_CHECK == ON)
    kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
}

static RK_ERR kTaskPoolInit_(ULONG const nTcbs)
{
    RK_CR_AREA
    RK_CR_ENTER

    if (RK_gTaskPoolInit == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_DOUBLE_INIT);
    }

    if ((nTcbs < RK_N_SYSTASKS) || (nTcbs > RK_NTHREADS))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    RK_ERR err =
        kMemPartitionInitGlobalScope(&RK_gTaskPool, RK_gTcbs,
                                     sizeof(RK_TCB), nTcbs);
    if (err == RK_ERR_SUCCESS)
    {
        kTraceNameObject(&RK_gTaskPool, "TCBPool");
        RK_gTaskPoolInit = RK_TRUE;
    }
    RK_CR_EXIT
    return (err);
}
/* initialises a task control block */
static RK_ERR kTaskInitTcb_(RK_TCB *const tcbPtr, RK_TID const tid,
                            RK_TASKENTRY const taskFunc, VOID *argsPtr,
                            RK_STACK *const stackBufPtr, ULONG const stackSize,
                            ULONG const savedControl,
                            RK_BOOL const protectedTask,
                            RK_TASK_MEMORY const *const memoryPtr)
{
    RK_ERR err;

    RK_MEMSET(tcbPtr, 0, sizeof(RK_TCB));

    if (protectedTask == RK_TRUE)
    {
        err = kMpuTaskMemoryReserve(tcbPtr, memoryPtr);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
    }

    if (kInitStack_(stackBufPtr, stackSize, taskFunc, argsPtr) !=
        RK_ERR_SUCCESS)
    {
        if (protectedTask == RK_TRUE)
        {
            kMpuTaskMemoryRelease(tcbPtr);
        }
        return (RK_ERR_ERROR);
    }

    tcbPtr->stackBufPtr = stackBufPtr;
    tcbPtr->sp = &stackBufPtr[stackSize - R4_OFFSET];
    tcbPtr->stackSize = stackSize;
    tcbPtr->status = RK_TCB_INITIALISED;
    tcbPtr->tid = tid;
    tcbPtr->savedLR = 0xFFFFFFFDU;
    tcbPtr->wakeTime = 0UL;
    tcbPtr->overrunCount = 0UL;
    tcbPtr->init = RK_TRUE;

    tcbPtr->savedControl = savedControl;

    if (protectedTask == RK_TRUE)
    {
        err = kMpuBuildRegion(
            &tcbPtr->mpuRegion[RK_MPU_REGION_DOMAIN_RAM],
            RK_MPU_REGION_DOMAIN_RAM,
            (ULONG)(UINTPTR)memoryPtr->regionBasePtr,
            memoryPtr->regionBytes,
            RK_MPU_ATTR_USER_SRAM);

        if (err != RK_ERR_SUCCESS)
        {
            kMpuTaskMemoryRelease(tcbPtr);
            RK_MEMSET(tcbPtr, 0, sizeof(RK_TCB));
            return (err);
        }

        err = kMpuBuildRegion(
            &tcbPtr->mpuRegion[RK_MPU_REGION_TASK_STACK],
            RK_MPU_REGION_TASK_STACK,
            (ULONG)(UINTPTR)memoryPtr->stackBasePtr,
            memoryPtr->stackWords * (ULONG)sizeof(RK_STACK),
            RK_MPU_ATTR_USER_SRAM);

        if (err != RK_ERR_SUCCESS)
        {
            kMpuTaskMemoryRelease(tcbPtr);
            RK_MEMSET(tcbPtr, 0, sizeof(RK_TCB));
            return (err);
        }

        err = kMpuTaskAttachSharedRegions(tcbPtr);

        if (err != RK_ERR_SUCCESS)
        {
            kMpuTaskMemoryRelease(tcbPtr);
            RK_MEMSET(tcbPtr, 0, sizeof(RK_TCB));
            return (err);
        }

    }

#if (RK_CONF_MESG_QUEUE == ON)
    tcbPtr->mesgQueueRecvBufPtr = NULL;
#endif
#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
    tcbPtr->asynchMesgInit = RK_FALSE;
    kListInit(&tcbPtr->asynchMesgQueue);
    kListInit(&tcbPtr->asynchMesgWaiters);
    kListInit(&tcbPtr->asynchMesgOwnedList);
    tcbPtr->asynchMesgWaitSenderPtr = NULL;
    tcbPtr->asynchMesgWaitDestPtr = NULL;
    tcbPtr->asynchMesgAllocDestPtr = NULL;
    tcbPtr->asynchMesgWaitStatus = RK_ERR_SUCCESS;
#if (RK_CONF_ASYNCH_COPY_MESG == ON)
    tcbPtr->asynchCopyMesgInit = RK_FALSE;
    kListInit(&tcbPtr->asynchCopyMesgQueue);
    kListInit(&tcbPtr->asynchCopyMesgWaiters);
    tcbPtr->asynchCopyMesgWaitSenderPtr = NULL;
    tcbPtr->asynchCopyMesgRecvBufPtr = NULL;
    tcbPtr->asynchCopyMesgRecvBufBytes = 0UL;
    tcbPtr->asynchCopyMesgRecvBytesPtr = NULL;
    tcbPtr->asynchCopyMesgRecvStatus = RK_ERR_SUCCESS;
#endif
#endif
#if (RK_CONF_SYNCH_MESG == ON)
    tcbPtr->synchMesgMaxBytes = 0UL;
    tcbPtr->synchMesgPendingPtr = NULL;
    tcbPtr->synchMesgPendingSenderPtr = NULL;
    tcbPtr->synchMesgRecvBufPtr = NULL;
    tcbPtr->synchMesgRecvBytesPtr = NULL;
    tcbPtr->synchMesgRecvStatus = RK_ERR_SUCCESS;
    kListInit(&tcbPtr->synchMesgSenders);
    tcbPtr->synchMesgPtr = NULL;
    tcbPtr->synchMesgBytes = 0UL;
    tcbPtr->synchMesgStatus = RK_ERR_SUCCESS;
    tcbPtr->synchMesgReceiverPtr = NULL;
    kListInit(&tcbPtr->synchMesgCallers);
    kListInit(&tcbPtr->synchMesgAcceptWaiters);
    tcbPtr->synchMesgActiveCallerPtr = NULL;
    tcbPtr->synchMesgActiveCallerPrio = tcbPtr->prioNominal;
    tcbPtr->synchMesgCallReplyBufPtr = NULL;
    tcbPtr->synchMesgCallReplyBytesPtr = NULL;
    tcbPtr->synchMesgCallReplyMaxBytes = 0UL;
    tcbPtr->synchMesgCallState = RK_SYNCH_CALL_IDLE;
#endif


#if (RK_CONF_MUTEX == ON)
    kListInit(&tcbPtr->ownedMutexList);
    tcbPtr->waitingForMutexPtr = NULL;
#endif

    return (RK_ERR_SUCCESS);
}
/* grabs a task control block from the task memory pool */
static RK_ERR
kTaskCreateFromPool_(RK_TASK_HANDLE *taskHandlePtr, RK_TASKENTRY const taskFunc,
                     VOID *argsPtr, RK_STRING taskName,
                     RK_STACK *const stackBufPtr, ULONG const stackSize,
                     RK_PRIO const priority, RK_OPTION const preempt,
                     RK_TID const requestedTid,
                     ULONG const savedControl,
                     RK_BOOL const protectedTask,
                     RK_TASK_MEMORY const *const memoryPtr)
{
    RK_TID tid = requestedTid;
    if (tid >= RK_NTHREADS)
    {
        tid = kTaskFindFreeTid_();
    }

    if ((tid >= RK_NTHREADS) || (RK_gTaskHandleByPid[tid] != NULL) ||
        (RK_gTaskQuarantineByPid[tid] == RK_TRUE))
    {
        return (RK_ERR_TASK_POOL_EMPTY);
    }

    RK_TCB *const newTcbPtr = &RK_gTcbs[tid];
    RK_ERR err = kTaskPoolReserveBlock_(newTcbPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kTaskInitTcb_(newTcbPtr, tid, taskFunc, argsPtr, stackBufPtr,
                        stackSize, savedControl, protectedTask, memoryPtr);
    if (err != RK_ERR_SUCCESS)
    {
        kMemPartitionFree(&RK_gTaskPool, newTcbPtr);
        return (err);
    }

    newTcbPtr->priority = priority;
    newTcbPtr->prioNominal = priority;
    newTcbPtr->preempt = preempt;
    kWriteTaskName_(newTcbPtr, taskName);

    err = kTaskRegister_(newTcbPtr, tid);
    if (err != RK_ERR_SUCCESS)
    {
        if (protectedTask == RK_TRUE)
        {
            kMpuTaskMemoryRelease(newTcbPtr);
        }
        RK_MEMSET(newTcbPtr, 0, sizeof(RK_TCB));
        kMemPartitionFree(&RK_gTaskPool, newTcbPtr);
        return (err);
    }

    *taskHandlePtr = kTaskHandleFromTcb(newTcbPtr);
    pPid += 1U;

    if (RK_gRunPtr != NULL)
    {
        RK_ERR readyErr = kReadySwtch(newTcbPtr);
        if (readyErr < 0)
        {
            if (protectedTask == RK_TRUE)
            {
                kMpuTaskMemoryRelease(newTcbPtr);
            }
            kTaskRegistryInvalidate_(newTcbPtr);
            RK_MEMSET(newTcbPtr, 0, sizeof(RK_TCB));
            kMemPartitionFree(&RK_gTaskPool, newTcbPtr);
            RK_gTaskDynStackPartByPid[tid] = NULL;
            *taskHandlePtr = NULL;
            pPid -= 1U;
            return (readyErr);
        }
    }

    return (RK_ERR_SUCCESS);
}

static RK_ERR kTaskEnsureSystemTasks_(VOID)
{
    RK_ERR err;

    if (RK_gSystemTasksInit == RK_TRUE)
    {
        return (RK_ERR_SUCCESS);
    }

    err = kTaskCreateFromPool_
    (
        &RK_gIdleTaskHandle, IdleTask, RK_NO_ARGS, "IdlTask", RK_gIdleStack,
        RK_CONF_IDLE_STACKSIZE, idleTaskPrio, RK_PREEMPT, RK_IDLETASK_ID,
        RK_SYSTEM_TASK_CONTROL, RK_SYSTEM_TASK_PROTECTED, NULL
    );
    if (err != RK_ERR_SUCCESS)
    {
        K_PANIC("Failed to create idle system task");
        return (err);
    }

    err = kTaskCreateFromPool_
    (
        &RK_gPostProcTaskHandle, PostProcSysTask, RK_NO_ARGS, "PostProc",
        RK_gPostProcStack, RK_CONF_POSTPROC_STACKSIZE, 0U, RK_NO_PREEMPT,
        RK_POSTPROC_TASK_ID, RK_SYSTEM_TASK_CONTROL,
        RK_SYSTEM_TASK_PROTECTED, NULL
    );
    if (err != RK_ERR_SUCCESS)
    {
        K_PANIC("Failed to create post-processing system task");
        return (err);
    }

    RK_TCB const *const postProcPtr =
        RK_gTaskHandleByPid[RK_POSTPROC_TASK_ID];
    if ((postProcPtr == NULL) ||
        ((postProcPtr->savedControl & 0x1UL) != 0UL))
    {
        K_PANIC("PostProc system task must be privileged");
        return (RK_ERR_INVALID_OBJ);
    }

    RK_gSystemTasksInit = RK_TRUE;
    return (RK_ERR_SUCCESS);
}
#if ((RK_CONF_DYNAMIC_TASK == ON) && (RK_CONF_ASYNCH_MESG == ON) &&          \
     (RK_CONF_MESG_QUEUE == ON))
static RK_BOOL kTaskReferencedByAsynchMesg_(RK_TCB const *taskPtr)
{
    for (UINT i = 0U; i < RK_NTHREADS; i++)
    {
        RK_TCB const *const receiverPtr = RK_gTaskHandleByPid[i];
        if ((receiverPtr == NULL) || (receiverPtr->init != RK_TRUE))
        {
            continue;
        }

        if (receiverPtr->asynchMesgWaitSenderPtr == taskPtr)
        {
            return (RK_TRUE);
        }
#if (RK_CONF_ASYNCH_COPY_MESG == ON)
        if (receiverPtr->asynchCopyMesgWaitSenderPtr == taskPtr)
        {
            return (RK_TRUE);
        }
#endif

        RK_NODE const *nodePtr =
            receiverPtr->asynchMesgQueue.listDummy.nextPtr;
        while (nodePtr != &receiverPtr->asynchMesgQueue.listDummy)
        {
            RK_MESG const *const mesgPtr =
                K_GET_CONTAINER_ADDR(nodePtr, RK_MESG, mesgNode);
            if ((mesgPtr->sender == kTaskHandleFromTcb(taskPtr)) &&
                (mesgPtr->senderPid == taskPtr->tid))
            {
                return (RK_TRUE);
            }

            nodePtr = nodePtr->nextPtr;
            RK_BARRIER
        }
    }

    return (RK_FALSE);
}
#endif

#if (RK_CONF_DYNAMIC_TASK == ON)
/* checks if a task can be terminated without affecting progress */
static RK_BOOL kTaskHasDependents_(RK_TCB const *taskPtr)
{
#if (RK_CONF_SYNCH_MESG == ON)
    if ((taskPtr->synchMesgPendingPtr != NULL) ||
        (taskPtr->synchMesgPendingSenderPtr != NULL) ||
        (taskPtr->synchMesgRecvBufPtr != NULL) ||
        (taskPtr->synchMesgSenders.size > 0U) ||
        (taskPtr->synchMesgReceiverPtr != NULL) ||
        (taskPtr->synchMesgCallers.size > 0U) ||
        (taskPtr->synchMesgAcceptWaiters.size > 0U) ||
        (taskPtr->synchMesgActiveCallerPtr != NULL) ||
        (taskPtr->synchMesgCallState != RK_SYNCH_CALL_IDLE))
    {
        return (RK_TRUE);
    }
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
    /*
     * Owned messages may still be applying a pool ceiling and must be freed or
     * transferred before the task can be destroyed safely.
     */
    if ((taskPtr->asynchMesgQueue.size > 0U) ||
        (taskPtr->asynchMesgWaiters.size > 0U) ||
        (taskPtr->asynchMesgOwnedList.size > 0U) ||
        (taskPtr->asynchMesgWaitSenderPtr != NULL) ||
        (taskPtr->asynchMesgWaitDestPtr != NULL) ||
        (taskPtr->asynchMesgAllocDestPtr != NULL))
    {
        return (RK_TRUE);
    }

#if (RK_CONF_ASYNCH_COPY_MESG == ON)
    if ((taskPtr->asynchCopyMesgQueue.size > 0U) ||
        (taskPtr->asynchCopyMesgWaiters.size > 0U) ||
        (taskPtr->asynchCopyMesgWaitSenderPtr != NULL) ||
        (taskPtr->asynchCopyMesgRecvBufPtr != NULL) ||
        (taskPtr->asynchCopyMesgRecvBytesPtr != NULL))
    {
        return (RK_TRUE);
    }
#endif

    if (kTaskReferencedByAsynchMesg_(taskPtr) == RK_TRUE)
    {
        return (RK_TRUE);
    }
#endif

    K_UNUSE(taskPtr);

    return (RK_FALSE);
}
#endif

static RK_BOOL kTaskNodeLinked_(RK_TCB const *const taskPtr)
{
    return (((taskPtr != NULL) && (taskPtr->tcbNode.nextPtr != NULL) &&
             (taskPtr->tcbNode.prevPtr != NULL))
                ? RK_TRUE
                : RK_FALSE);
}

RK_ERR kTaskSignalReady(RK_TCB *const taskPtr)
{
    RK_BOOL waitObjectDetached = RK_FALSE;

    if ((taskPtr == NULL) || (taskPtr->init != RK_TRUE))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if ((taskPtr->status == RK_READY) || (taskPtr->status == RK_RUNNING))
    {
        return (RK_ERR_SUCCESS);
    }

    switch (taskPtr->status)
    {
        case RK_SLEEPING:
        case RK_SLEEPING_EV_FLAG:
        case RK_BLOCKED:
        case RK_SENDING:
        case RK_RECEIVING:
        case RK_SLEEPING_DELAY:
        case RK_SLEEPING_RELEASE:
        case RK_SLEEPING_UNTIL:
        case RK_SLEEPQ_BLOCKED:
            break;

        default:
            return (RK_ERR_TASK_INVALID_ST);
    }

#if (RK_CONF_SYNCH_MESG == ON)
    /*
     * Synchronous-message waits own more than the generic TCB queue link. A
     * queued sender/caller points back to its receiver/server and may be
     * contributing effective priority. Let that object detach the wait before
     * the syscall continuation is completed as signal-interrupted.
     */
    if ((taskPtr->syscallNumber == RK_SYSCALL_SYNCH_SEND_WAIT) &&
        (taskPtr->synchMesgReceiverPtr != NULL) &&
        (taskPtr->synchMesgCallState == RK_SYNCH_CALL_IDLE))
    {
        kSynchMesgSignalSend(taskPtr);
        waitObjectDetached = RK_TRUE;
    }
    else if ((taskPtr->syscallNumber == RK_SYSCALL_SYNCH_MESG_CALL) &&
             ((taskPtr->synchMesgReceiverPtr != NULL) ||
              (taskPtr->synchMesgCallState != RK_SYNCH_CALL_IDLE)))
    {
        kSynchMesgSignalCall(taskPtr);
        waitObjectDetached = RK_TRUE;
    }
#endif

    if ((waitObjectDetached == RK_FALSE) &&
        (taskPtr->timeoutNode.waitingQueuePtr != NULL))
    {
        if (kTaskNodeLinked_(taskPtr) == RK_TRUE)
        {
            RK_TCB *remPtr = taskPtr;
            kTCBQRem(taskPtr->timeoutNode.waitingQueuePtr, &remPtr);
        }
#if (RK_CONF_MUTEX == ON)
        if (taskPtr->waitingForMutexPtr != NULL)
        {
            kMutexWaiterRemoved(taskPtr);
        }
#endif
    }

    if (kTimeoutNodeIsArmed(&taskPtr->timeoutNode) == RK_TRUE)
    {
        RK_ERR const err = kTimeoutNodeDisarm(&taskPtr->timeoutNode);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
    }
    else
    {
        kTimeoutNodeReset(&taskPtr->timeoutNode);
    }

    taskPtr->timeOut = RK_FALSE;
    kSyscallTaskSignal(taskPtr);
    return (kReadySwtch(taskPtr));
}

#if (RK_CONF_MUTEX == ON)
static VOID kTaskFaultReleaseOwnedMutexes_(RK_TCB *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return;
    }

    while (taskPtr->ownedMutexList.size > 0UL)
    {
        RK_NODE *const nodePtr = taskPtr->ownedMutexList.listDummy.nextPtr;
        RK_MUTEX *const mtxPtr =
            K_GET_CONTAINER_ADDR(nodePtr, RK_MUTEX, mutexNode);

        RK_ERR err = kListRemove(&taskPtr->ownedMutexList, nodePtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        if ((err != RK_ERR_SUCCESS) || (mtxPtr == NULL) ||
            (mtxPtr->ownerPtr != taskPtr))
        {
            continue;
        }

        mtxPtr->ownerFaulted = RK_TRUE;
        mtxPtr->ownerPtr = NULL;
        mtxPtr->lock = RK_FALSE;

        while (mtxPtr->waitingQueue.size > 0UL)
        {
            RK_TCB *waiterPtr = NULL;

            err = kTCBQDeq(&mtxPtr->waitingQueue, &waiterPtr);
            K_ASSERT(err == RK_ERR_SUCCESS);
            if ((err != RK_ERR_SUCCESS) || (waiterPtr == NULL))
            {
                break;
            }

            if (kTimeoutNodeIsArmed(&waiterPtr->timeoutNode) == RK_TRUE)
            {
                kTimeoutNodeDisarm(&waiterPtr->timeoutNode);
            }
            else
            {
                kTimeoutNodeReset(&waiterPtr->timeoutNode);
            }

            waiterPtr->waitingForMutexPtr = NULL;
            waiterPtr->timeOut = RK_FALSE;
            waiterPtr->syscallWakeResult = RK_ERR_MUTEX_OWNER_FAULTED;
            kReadySwtch(waiterPtr);
        }
    }

    kListInit(&taskPtr->ownedMutexList);
    taskPtr->waitingForMutexPtr = NULL;
}
#endif

static RK_ERR kTaskFaultUnlink_(RK_TCB *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (taskPtr->status == RK_READY)
    {
        if (kTaskNodeLinked_(taskPtr) == RK_TRUE)
        {
            RK_TCB *remPtr = taskPtr;
            kTCBQRem(&RK_gReadyQueue[taskPtr->priority], &remPtr);
        }
    }
    else if (taskPtr->timeoutNode.waitingQueuePtr != NULL)
    {
        if (kTaskNodeLinked_(taskPtr) == RK_TRUE)
        {
            RK_TCB *remPtr = taskPtr;
            kTCBQRem(taskPtr->timeoutNode.waitingQueuePtr, &remPtr);
        }
    }

    if (kTimeoutNodeIsArmed(&taskPtr->timeoutNode) == RK_TRUE)
    {
        RK_ERR const err = kTimeoutNodeDisarm(&taskPtr->timeoutNode);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
    }
    else
    {
        kTimeoutNodeReset(&taskPtr->timeoutNode);
    }

    return (RK_ERR_SUCCESS);
}

RK_ERR kTaskFaultTerminate(RK_TCB *const taskPtr)
{
    RK_CR_AREA
    RK_CR_ENTER

    if ((taskPtr == NULL) || (taskPtr != RK_gRunPtr) ||
        (taskPtr->init != RK_TRUE) || (taskPtr->status != RK_RUNNING))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    RK_TID const tid = taskPtr->tid;
    if ((tid <= RK_POSTPROC_TASK_ID) || (tid >= RK_NTHREADS) ||
        (RK_gTaskHandleByPid[tid] != taskPtr))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    kSyscallTaskClear(taskPtr);

    RK_ERR const err = kPostProcFaultCleanupPend(tid);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    taskPtr->status = RK_TASK_FAULT_PENDING;
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kTaskFaultCleanup(RK_TID const tid)
{
    RK_CR_AREA
    RK_CR_ENTER

    if ((tid < RK_N_SYSTASKS) || (tid >= RK_NTHREADS))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    RK_TCB *const taskPtr = RK_gTaskHandleByPid[tid];
    if (taskPtr == NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    if ((taskPtr == RK_gRunPtr) || (taskPtr->tid != tid))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    RK_TID const slotPid = taskPtr->tid;
    RK_BOOL const tcbInPool = kTaskPointerInPool_(taskPtr);
    RK_STACK *stackBufPtr = NULL;
    RK_MEM_PARTITION *stackMemPtr = RK_gTaskDynStackPartByPid[slotPid];
    RK_BOOL const quarantineSlot =
        (stackMemPtr == NULL) ? RK_TRUE : RK_FALSE;

    if (stackMemPtr != NULL)
    {
        stackBufPtr = taskPtr->stackBufPtr;
    }

    kTaskRegistryInvalidate_(taskPtr);
    RK_gTaskDynStackPartByPid[slotPid] = NULL;
    if (quarantineSlot == RK_TRUE)
    {
        RK_gTaskQuarantineByPid[slotPid] = RK_TRUE;
    }

    RK_ERR err = kTaskFaultUnlink_(taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

#if (RK_CONF_MUTEX == ON)
    if (taskPtr->waitingForMutexPtr != NULL)
    {
        RK_MUTEX *const waitMtxPtr = taskPtr->waitingForMutexPtr;
        taskPtr->waitingForMutexPtr = NULL;
        if (waitMtxPtr->ownerPtr != NULL)
        {
            kTaskUpdateEffectivePrioChain(waitMtxPtr->ownerPtr);
        }
    }
    kTaskFaultReleaseOwnedMutexes_(taskPtr);
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
    kMesgTaskCleanup(taskPtr);
#endif

#if (RK_CONF_SYNCH_MESG == ON)
    kSynchMesgTaskCleanup(taskPtr);
#endif

#if (RK_CONF_MRM == ON)
    err = kMRMTaskCleanup(taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
#endif

#if (RK_CONF_MESG_QUEUE == ON)
    taskPtr->mesgQueueRecvBufPtr = NULL;
#endif

    kSyscallTaskClear(taskPtr);
    kSignalTaskCleanup(taskPtr);
    taskPtr->flagsCurr = 0UL;
    taskPtr->flagsOpt = 0UL;
    taskPtr->flagsReq = 0UL;
    taskPtr->timeOut = RK_FALSE;

    kMpuTaskMemoryRelease(taskPtr);

    taskPtr->status = RK_TASK_TERMINATED;
    taskPtr->init = RK_FALSE;

    if ((stackBufPtr != NULL) && (stackMemPtr != NULL))
    {
        err = kMemPartitionFree(stackMemPtr, stackBufPtr);
        if (err != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (err);
        }
    }

    if ((tcbInPool == RK_TRUE) && (quarantineSlot == RK_FALSE))
    {
        RK_MEMSET(taskPtr, 0, sizeof(RK_TCB));
        err = kMemPartitionFree(&RK_gTaskPool, taskPtr);
    }
    else
    {
        taskPtr->tid = slotPid;
        taskPtr->status = RK_TASK_TERMINATED;
    }

    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    if (pPid > 0U)
    {
        pPid -= 1U;
    }

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kDomainInit(RK_DOMAIN *const domainPtr,
                   BYTE *const regionBasePtr,
                   ULONG const regionBytes,
                   RK_STRING domainName)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_DOMAIN_INIT,
                                        (ULONG)(UINTPTR)domainPtr,
                                        (ULONG)(UINTPTR)regionBasePtr,
                                        regionBytes,
                                        (ULONG)(UINTPTR)domainName));
    }

    if ((domainPtr == NULL) || (regionBasePtr == NULL) ||
        (domainName == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (kMpuLayoutIsFinalized() == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PHASE);
#endif
        return (RK_ERR_INVALID_PHASE);
    }

    if (domainPtr->init == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_DOUBLE_INIT);
#endif
        return (RK_ERR_OBJ_DOUBLE_INIT);
    }

    RK_MEMSET(domainPtr, 0, sizeof(RK_DOMAIN));
    kWriteName_(domainPtr->domainName, domainName);
    domainPtr->regionBasePtr = regionBasePtr;
    domainPtr->regionBytes = regionBytes;
    domainPtr->init = RK_FALSE;

    RK_ERR const err = kMpuDomainMemoryReserve(domainPtr);
    if (err != RK_ERR_SUCCESS)
    {
        domainPtr->regionBasePtr = NULL;
        domainPtr->regionBytes = 0UL;
        return (err);
    }

    domainPtr->init = RK_TRUE;
    return (RK_ERR_SUCCESS);
}

static RK_BOOL kDomainAllocAlignValid_(ULONG const alignBytes)
{
    return (((alignBytes != 0UL) &&
             ((alignBytes & (alignBytes - 1UL)) == 0UL)) ?
                RK_TRUE :
                RK_FALSE);
}

VOID *kDomainAlloc(RK_DOMAIN *const domainPtr,
                   ULONG const nBytes,
                   ULONG const alignBytes)
{
    ULONG alignedOffset;
    ULONG nextOffset;
    ULONG const alignMask = alignBytes - 1UL;

    if (kSyscallRequired() == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PHASE);
#endif
        return (NULL);
    }

    if ((domainPtr == NULL) || (domainPtr->regionBasePtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (NULL);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (NULL);
    }

    if (domainPtr->init != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NOT_INIT);
#endif
        return (NULL);
    }

    if (kMpuLayoutIsFinalized() == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PHASE);
#endif
        return (NULL);
    }

    if ((nBytes == 0UL) ||
        (kDomainAllocAlignValid_(alignBytes) == RK_FALSE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (NULL);
    }

    if (domainPtr->allocBytes > (RK_ULONG_MAX - alignMask))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (NULL);
    }

    alignedOffset = (domainPtr->allocBytes + alignMask) & ~alignMask;
    if ((alignedOffset > domainPtr->regionBytes) ||
        (nBytes > (domainPtr->regionBytes - alignedOffset)))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (NULL);
    }

    nextOffset = alignedOffset + nBytes;
    if ((nextOffset < alignedOffset) || (nextOffset > domainPtr->regionBytes))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (NULL);
    }

    domainPtr->allocBytes = nextOffset;
    return ((VOID *)&domainPtr->regionBasePtr[alignedOffset]);
}

RK_ERR kSharedRegionInit(RK_SHARED_REGION *const regionPtr,
                         BYTE *const regionBasePtr,
                         ULONG const regionBytes)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SHARED_REGION_INIT,
                                        (ULONG)(UINTPTR)regionPtr,
                                        (ULONG)(UINTPTR)regionBasePtr,
                                        regionBytes, 0UL));
    }

    if ((regionPtr == NULL) || (regionBasePtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (kMpuLayoutIsFinalized() == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PHASE);
#endif
        return (RK_ERR_INVALID_PHASE);
    }

    if (regionPtr->init == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_DOUBLE_INIT);
#endif
        return (RK_ERR_OBJ_DOUBLE_INIT);
    }

    RK_MEMSET(regionPtr, 0, sizeof(RK_SHARED_REGION));
    regionPtr->regionBasePtr = regionBasePtr;
    regionPtr->regionBytes = regionBytes;
    regionPtr->init = RK_FALSE;

    RK_ERR const err = kMpuSharedRegionMemoryReserve(regionPtr);
    if (err != RK_ERR_SUCCESS)
    {
        regionPtr->regionBasePtr = NULL;
        regionPtr->regionBytes = 0UL;
        return (err);
    }

    regionPtr->init = RK_TRUE;
    return (RK_ERR_SUCCESS);
}

RK_ERR kDomainMapSharedRegion(RK_DOMAIN *const domainPtr,
                              RK_SHARED_REGION *const regionPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_DOMAIN_MAP_SHARED_REGION, (ULONG)(UINTPTR)domainPtr,
            (ULONG)(UINTPTR)regionPtr, 0UL, 0UL));
    }

    if ((domainPtr == NULL) || (regionPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (kMpuLayoutIsFinalized() == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PHASE);
#endif
        return (RK_ERR_INVALID_PHASE);
    }

    if ((domainPtr->init != RK_TRUE) || (regionPtr->init != RK_TRUE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NOT_INIT);
#endif
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (domainPtr->taskCount != 0UL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_OBJ);
#endif
        return (RK_ERR_INVALID_OBJ);
    }

    for (UINT idx = 0U; idx < RK_CONF_DOMAIN_SHARED_REGIONS; idx++)
    {
        if (domainPtr->sharedRegionPtr[idx] == regionPtr)
        {
#if (RK_CONF_ERR_CHECK == ON)
            kErrHandler(RK_FAULT_OBJ_DOUBLE_INIT);
#endif
            return (RK_ERR_OBJ_DOUBLE_INIT);
        }
    }

    for (UINT idx = 0U; idx < RK_CONF_DOMAIN_SHARED_REGIONS; idx++)
    {
        if (domainPtr->sharedRegionPtr[idx] == NULL)
        {
            domainPtr->sharedRegionPtr[idx] = regionPtr;
            return (RK_ERR_SUCCESS);
        }
    }

#if (RK_CONF_ERR_CHECK == ON)
    kErrHandler(RK_FAULT_INVALID_OBJ);
#endif
    return (RK_ERR_INVALID_OBJ);
}

RK_ERR kTaskInit(RK_TASK_HANDLE *taskHandlePtr, const RK_TASKENTRY taskFunc,
                 VOID *argsPtr, RK_STRING taskName,
                 RK_STACK *const stackBufPtr, const ULONG stackSize,
                 const RK_PRIO priority, const RK_OPTION preempt)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        RK_TASK_INIT_SYSCALL_ARGS syscallArgs;

        syscallArgs.taskHandlePtr = taskHandlePtr;
        syscallArgs.taskFunc = taskFunc;
        syscallArgs.argsPtr = argsPtr;
        syscallArgs.taskName = taskName;
        syscallArgs.stackBufPtr = stackBufPtr;
        syscallArgs.stackSize = stackSize;
        syscallArgs.priority = priority;
        syscallArgs.preempt = preempt;
        syscallArgs.domainPtr = NULL;

        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TASK_INIT,
                                        (ULONG)(UINTPTR)&syscallArgs,
                                        0UL, 0UL, 0UL));
    }

    RK_TASK_MEMORY defaultTaskMemory;
    RK_BOOL protectedTask = RK_FALSE;
    ULONG savedControl = RK_CONTROL_PSP_PRIVILEGED;

    if ((taskHandlePtr == NULL) || (taskFunc == NULL) || (taskName == NULL) ||
        (stackBufPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((preempt != RK_PREEMPT) && (preempt != RK_NO_PREEMPT))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }
    /* remember higher number -> lower prio */
    if (priority > lowestPrio)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_TASK_INVALID_PRIO);
#endif
        return (RK_ERR_INVALID_PRIO);
    }

    if (kTaskStackGeometryValid_(stackBufPtr, stackSize) == RK_FALSE)
    {
        kTaskStackGeometryFault_(taskName, stackBufPtr, stackSize);
        return (RK_ERR_INVALID_PARAM);
    }

    RK_ERR err = kApplicationDomainEnsureInit();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    defaultTaskMemory.regionBasePtr = RK_gApplicationDomain.regionBasePtr;
    defaultTaskMemory.regionBytes = RK_gApplicationDomain.regionBytes;
    defaultTaskMemory.stackBasePtr = stackBufPtr;
    defaultTaskMemory.stackWords = stackSize;
    defaultTaskMemory.domainPtr = &RK_gApplicationDomain;
    protectedTask = RK_TRUE;
    savedControl = RK_CONTROL_PSP_UNPRIVILEGED;

    RK_CR_AREA
    RK_CR_ENTER

    err = kTaskPoolEnsureInit_(RK_NTHREADS);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kTaskEnsureSystemTasks_();
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kTaskCreateFromPool_(taskHandlePtr, taskFunc, argsPtr, taskName,
                               stackBufPtr, stackSize,
                               priority, preempt, (RK_TID)RK_NTHREADS,
                               savedControl, protectedTask,
                               &defaultTaskMemory);
    RK_CR_EXIT
    return (err);
}

RK_ERR kTaskInitIsolated(RK_TASK_HANDLE *taskHandlePtr,
                         const RK_TASKENTRY taskFunc,
                         VOID *argsPtr,
                         RK_STRING taskName,
                         RK_STACK *const stackBufPtr,
                         const ULONG stackSize,
                         const RK_PRIO priority,
                         const RK_OPTION preempt)
{
    if (kSyscallRequired() == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PHASE);
#endif
        return (RK_ERR_INVALID_PHASE);
    }

    RK_TASK_MEMORY isolatedTaskMemory;

    if ((taskHandlePtr == NULL) || (taskFunc == NULL) || (taskName == NULL) ||
        (stackBufPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((preempt != RK_PREEMPT) && (preempt != RK_NO_PREEMPT))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }

    if (priority > lowestPrio)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_TASK_INVALID_PRIO);
#endif
        return (RK_ERR_INVALID_PRIO);
    }

    if (kTaskStackGeometryValid_(stackBufPtr, stackSize) == RK_FALSE)
    {
        kTaskStackGeometryFault_(taskName, stackBufPtr, stackSize);
        return (RK_ERR_INVALID_PARAM);
    }

    isolatedTaskMemory.regionBasePtr = (BYTE *)stackBufPtr;
    isolatedTaskMemory.regionBytes = stackSize * (ULONG)sizeof(RK_STACK);
    isolatedTaskMemory.stackBasePtr = stackBufPtr;
    isolatedTaskMemory.stackWords = stackSize;
    isolatedTaskMemory.domainPtr = NULL;

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR err = kTaskPoolEnsureInit_(RK_NTHREADS);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kTaskEnsureSystemTasks_();
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kTaskCreateFromPool_(taskHandlePtr, taskFunc, argsPtr, taskName,
                               stackBufPtr, stackSize,
                               priority, preempt, (RK_TID)RK_NTHREADS,
                               RK_CONTROL_PSP_UNPRIVILEGED, RK_TRUE,
                               &isolatedTaskMemory);
    RK_CR_EXIT
    return (err);
}

RK_ERR kTaskInitDomain(RK_TASK_HANDLE *taskHandlePtr,
                       const RK_TASKENTRY taskFunc,
                       VOID *argsPtr,
                       RK_STRING taskName,
                       RK_STACK *const stackBufPtr,
                       const ULONG stackSize,
                       const RK_PRIO priority,
                       const RK_OPTION preempt,
                       RK_DOMAIN *const domainPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        RK_TASK_INIT_SYSCALL_ARGS syscallArgs;

        syscallArgs.taskHandlePtr = taskHandlePtr;
        syscallArgs.taskFunc = taskFunc;
        syscallArgs.argsPtr = argsPtr;
        syscallArgs.taskName = taskName;
        syscallArgs.stackBufPtr = stackBufPtr;
        syscallArgs.stackSize = stackSize;
        syscallArgs.priority = priority;
        syscallArgs.preempt = preempt;
        syscallArgs.domainPtr = domainPtr;

        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TASK_INIT_DOMAIN,
                                        (ULONG)(UINTPTR)&syscallArgs,
                                        0UL, 0UL, 0UL));
    }

    RK_TASK_MEMORY domainTaskMemory;

    if ((taskHandlePtr == NULL) || (taskFunc == NULL) || (taskName == NULL) ||
        (stackBufPtr == NULL) || (domainPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (domainPtr->init != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NOT_INIT);
#endif
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if ((preempt != RK_PREEMPT) && (preempt != RK_NO_PREEMPT))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }

    if (priority > lowestPrio)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_TASK_INVALID_PRIO);
#endif
        return (RK_ERR_INVALID_PRIO);
    }

    domainTaskMemory.regionBasePtr = domainPtr->regionBasePtr;
    domainTaskMemory.regionBytes = domainPtr->regionBytes;
    domainTaskMemory.stackBasePtr = stackBufPtr;
    domainTaskMemory.stackWords = stackSize;
    domainTaskMemory.domainPtr = domainPtr;

    if (kTaskStackGeometryValid_(stackBufPtr, stackSize) == RK_FALSE)
    {
        kTaskStackGeometryFault_(taskName, stackBufPtr, stackSize);
        return (RK_ERR_INVALID_PARAM);
    }

    if (kMpuTaskMemoryValid(&domainTaskMemory) == RK_FALSE)
    {
#if (RK_CONF_FAULT_PRINT_STDERR == ON)
        printf("TASK INIT INVALID MEMORY: task=%s domain=%s stack=0x%08lx "
               "words=%lu region=0x%08lx bytes=%lu\r\n",
               taskName,
               domainPtr->domainName,
               (ULONG)stackBufPtr,
               stackSize,
               (ULONG)domainPtr->regionBasePtr,
               domainPtr->regionBytes);
#endif
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR err = kTaskPoolEnsureInit_(RK_NTHREADS);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kTaskEnsureSystemTasks_();
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kTaskCreateFromPool_(taskHandlePtr, taskFunc, argsPtr, taskName,
                               stackBufPtr, stackSize,
                               priority, preempt, (RK_TID)RK_NTHREADS,
                               RK_CONTROL_PSP_UNPRIVILEGED, RK_TRUE,
                               &domainTaskMemory);
    RK_CR_EXIT
    return (err);
}

RK_ERR kTaskInitPrivileged(RK_TASK_HANDLE *taskHandlePtr,
                           const RK_TASKENTRY taskFunc,
                           VOID *argsPtr,
                           RK_STRING taskName,
                           RK_STACK *const stackBufPtr,
                           const ULONG stackSize,
                           const RK_PRIO priority,
                           const RK_OPTION preempt)
{
    if ((taskHandlePtr == NULL) || (taskFunc == NULL) || (taskName == NULL) ||
        (stackBufPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((preempt != RK_PREEMPT) && (preempt != RK_NO_PREEMPT))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }

    if (priority > lowestPrio)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_TASK_INVALID_PRIO);
#endif
        return (RK_ERR_INVALID_PRIO);
    }

    if (kTaskStackGeometryValid_(stackBufPtr, stackSize) == RK_FALSE)
    {
        kTaskStackGeometryFault_(taskName, stackBufPtr, stackSize);
        return (RK_ERR_INVALID_PARAM);
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR err = kTaskPoolEnsureInit_(RK_NTHREADS);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kTaskEnsureSystemTasks_();
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kTaskCreateFromPool_(taskHandlePtr, taskFunc, argsPtr, taskName,
                               stackBufPtr, stackSize, priority, preempt,
                               (RK_TID)RK_NTHREADS,
                               RK_CONTROL_PSP_PRIVILEGED, RK_FALSE, NULL);
    RK_CR_EXIT
    return (err);
}

RK_ERR kTaskInitProtected(RK_TCB *const taskPtr,
                          RK_TASKENTRY const taskFunc,
                          VOID *const argsPtr,
                          RK_PRIO const priority,
                          RK_TASK_MEMORY const *const memoryPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        RK_TASK_PROTECTED_INIT_SYSCALL_ARGS syscallArgs;

        syscallArgs.taskPtr = taskPtr;
        syscallArgs.taskFunc = taskFunc;
        syscallArgs.argsPtr = argsPtr;
        syscallArgs.priority = priority;
        syscallArgs.memoryPtr = memoryPtr;

        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_TASK_INIT_PROTECTED, (ULONG)(UINTPTR)&syscallArgs,
            0UL, 0UL, 0UL));
    }

    RK_TID tid;
    RK_BOOL poolReserved = RK_FALSE;
    RK_ERR err;

    if ((taskPtr == NULL) || (taskFunc == NULL) || (memoryPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (priority > lowestPrio)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_TASK_INVALID_PRIO);
#endif
        return (RK_ERR_INVALID_PRIO);
    }

    if (kTaskStackGeometryValid_(memoryPtr->stackBasePtr,
                                 memoryPtr->stackWords) == RK_FALSE)
    {
        kTaskStackGeometryFault_(taskPtr->taskName, memoryPtr->stackBasePtr,
                                 memoryPtr->stackWords);
        return (RK_ERR_INVALID_PARAM);
    }

    if (kMpuTaskMemoryValid(memoryPtr) == RK_FALSE)
    {
#if (RK_CONF_FAULT_PRINT_STDERR == ON)
        printf("TASK INIT INVALID MEMORY: tcb=0x%08lx stack=0x%08lx "
               "words=%lu region=0x%08lx bytes=%lu\r\n",
               (ULONG)taskPtr,
               (ULONG)memoryPtr->stackBasePtr,
               memoryPtr->stackWords,
               (ULONG)memoryPtr->regionBasePtr,
               memoryPtr->regionBytes);
#endif
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }

    if (taskPtr->init == RK_TRUE)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if (kTaskPointerInPool_(taskPtr) == RK_FALSE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_OBJ);
#endif
        return (RK_ERR_INVALID_OBJ);
    }

    RK_CR_AREA
    RK_CR_ENTER

    err = kTaskPoolEnsureInit_(RK_NTHREADS);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kTaskEnsureSystemTasks_();
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    UINTPTR const poolBegin = (UINTPTR)&RK_gTcbs[0];
    tid = (RK_TID)(((UINTPTR)taskPtr - poolBegin) / sizeof(RK_TCB));
    err = kTaskPoolReserveBlock_(taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    poolReserved = RK_TRUE;

    if ((tid >= RK_NTHREADS) || (RK_gTaskHandleByPid[tid] != NULL) ||
        (RK_gTaskQuarantineByPid[tid] == RK_TRUE))
    {
        if (poolReserved == RK_TRUE)
        {
            kMemPartitionFree(&RK_gTaskPool, taskPtr);
        }
        RK_CR_EXIT
        return (RK_ERR_TASK_POOL_EMPTY);
    }

    err = kTaskInitTcb_(taskPtr, tid, taskFunc, argsPtr,
                        memoryPtr->stackBasePtr, memoryPtr->stackWords,
                        RK_CONTROL_PSP_UNPRIVILEGED, RK_TRUE, memoryPtr);
    if (err != RK_ERR_SUCCESS)
    {
        if (poolReserved == RK_TRUE)
        {
            kMemPartitionFree(&RK_gTaskPool, taskPtr);
        }
        RK_CR_EXIT
        return (err);
    }

    taskPtr->priority = priority;
    taskPtr->prioNominal = priority;
    taskPtr->preempt = RK_PREEMPT;
    kWriteTaskName_(taskPtr, "ProtTsk");

    err = kTaskRegister_(taskPtr, tid);
    if (err != RK_ERR_SUCCESS)
    {
        kMpuTaskMemoryRelease(taskPtr);
        RK_MEMSET(taskPtr, 0, sizeof(RK_TCB));
        if (poolReserved == RK_TRUE)
        {
            kMemPartitionFree(&RK_gTaskPool, taskPtr);
        }
        RK_CR_EXIT
        return (err);
    }

    pPid += 1U;

    if (RK_gRunPtr != NULL)
    {
        RK_ERR readyErr = kReadySwtch(taskPtr);
        if (readyErr < 0)
        {
            kMpuTaskMemoryRelease(taskPtr);
            RK_gTaskHandleByPid[tid] = NULL;
            kTaskHandleGenerationBump_(tid);
            RK_gTaskDynStackPartByPid[tid] = NULL;
            RK_MEMSET(taskPtr, 0, sizeof(RK_TCB));
            if (poolReserved == RK_TRUE)
            {
                kMemPartitionFree(&RK_gTaskPool, taskPtr);
            }
            pPid -= 1U;
            RK_CR_EXIT
            return (readyErr);
        }
    }

    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

#if (RK_CONF_DYNAMIC_TASK == ON)
RK_ERR kTaskSpawn(RK_DYNAMIC_TASK_ATTR const *taskAttrPtr,
                  RK_TASK_HANDLE *taskHandlePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TASK_SPAWN,
                                        (ULONG)(UINTPTR)taskAttrPtr,
                                        (ULONG)(UINTPTR)taskHandlePtr,
                                        0UL, 0UL));
    }

    if ((taskAttrPtr == NULL) || (taskHandlePtr == NULL) ||
        (taskAttrPtr->taskFunc == NULL) || (taskAttrPtr->taskName == NULL) ||
        (taskAttrPtr->stackMemPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((taskAttrPtr->preempt != RK_PREEMPT) &&
        (taskAttrPtr->preempt != RK_NO_PREEMPT))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }

    if (taskAttrPtr->priority > lowestPrio)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_TASK_INVALID_PRIO);
#endif
        return (RK_ERR_INVALID_PRIO);
    }

    RK_MEM_PARTITION *stackMemPtr = taskAttrPtr->stackMemPtr;
    if ((stackMemPtr->objID != RK_MEMALLOC_KOBJ_ID) ||
        (stackMemPtr->init != RK_TRUE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_OBJ);
#endif
        return (RK_ERR_INVALID_OBJ);
    }

    RK_DOMAIN *domainPtr = taskAttrPtr->domainPtr;
    if (domainPtr == NULL)
    {
        if (RK_gRunPtr != NULL)
        {
            domainPtr = RK_gRunPtr->domainPtr;
        }
        else
        {
            RK_ERR const appDomainErr = kApplicationDomainEnsureInit();
            if (appDomainErr != RK_ERR_SUCCESS)
            {
                return (appDomainErr);
            }
            domainPtr = &RK_gApplicationDomain;
        }
    }

    if (domainPtr != NULL)
    {
        if (domainPtr->init != RK_TRUE)
        {
#if (RK_CONF_ERR_CHECK == ON)
            kErrHandler(RK_FAULT_OBJ_NOT_INIT);
#endif
            return (RK_ERR_OBJ_NOT_INIT);
        }

        if (kMpuDomainMemoryValid(domainPtr) == RK_FALSE)
        {
#if (RK_CONF_ERR_CHECK == ON)
            kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
            return (RK_ERR_INVALID_PARAM);
        }
    }

    ULONG const stackSize = (stackMemPtr->blkSize / RK_WORD_SIZE);
    if ((stackSize < RK_CONF_MIN_STACKSIZE) || ((stackSize & 1U) != 0U))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }

    RK_STACK *stackBufPtr = (RK_STACK *)kMemPartitionAlloc(stackMemPtr);
    if (stackBufPtr == NULL)
    {
        return (RK_ERR_TASK_POOL_EMPTY);
    }

    RK_BOOL locked = RK_FALSE;
    if ((RK_gRunPtr != NULL) && (RK_gRunPtr->preempt != RK_NO_PREEMPT))
    {
        kSchLock();
        locked = RK_TRUE;
    }

    *taskHandlePtr = NULL;
    RK_ERR err;
    if (domainPtr != NULL)
    {
        err = kTaskInitDomain(taskHandlePtr, taskAttrPtr->taskFunc,
                              taskAttrPtr->argsPtr, taskAttrPtr->taskName,
                              stackBufPtr, stackSize,
                              taskAttrPtr->priority, taskAttrPtr->preempt,
                              domainPtr);
    }
    else
    {
        err = kTaskInit(taskHandlePtr, taskAttrPtr->taskFunc,
                        taskAttrPtr->argsPtr, taskAttrPtr->taskName,
                        stackBufPtr, stackSize,
                        taskAttrPtr->priority, taskAttrPtr->preempt);
    }

    if (err == RK_ERR_SUCCESS)
    {
        RK_TCB *taskPtr = NULL;
        err = kTaskHandleResolve(*taskHandlePtr, &taskPtr);
        if ((err != RK_ERR_SUCCESS) || (taskPtr->tid >= RK_NTHREADS))
        {
            err = RK_ERR_ERROR;
        }
        else
        {
            RK_gTaskDynStackPartByPid[taskPtr->tid] = stackMemPtr;
        }
    }

    if (locked == RK_TRUE)
    {
        kSchUnlock();
    }

    if (err != RK_ERR_SUCCESS)
    {
        kMemPartitionFree(stackMemPtr, stackBufPtr);
    }
    return (err);
}


RK_ERR kTaskTerminateSelf(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TASK_TERMINATE_SELF,
                                        0UL, 0UL, 0UL, 0UL));
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((RK_gRunPtr == NULL) || (RK_gRunPtr->tid <= RK_POSTPROC_TASK_ID))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_OBJ);
#endif
        return (RK_ERR_INVALID_OBJ);
    }

    return (kTaskTerminateTcb(RK_gRunPtr));
}

static RK_ERR kTaskTerminateResolved_(RK_TCB *const taskPtr,
                                      RK_TASK_HANDLE *const taskHandlePtr)
{
    RK_CR_AREA
    RK_ERR err = RK_ERR_SUCCESS;
    RK_CR_ENTER

    if (taskPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (RK_gTaskPoolInit == RK_FALSE)
    {
        K_PANIC("Task pool not initialised");
        kErrHandler(RK_FAULT_TASK_POOL_NOT_INIT);
        RK_CR_EXIT
        return (RK_ERR_TASK_POOL_NOT_INIT);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (taskPtr->init != RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_NOT_INIT);
    }

    RK_TID const taskPid = taskPtr->tid;
    if ((taskPid <= RK_POSTPROC_TASK_ID) || (taskPid >= RK_NTHREADS))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    /* Only runtime-spawned tasks are terminable. */
    if (RK_gTaskDynStackPartByPid[taskPid] == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

#if (RK_CONF_MUTEX == ON)
    if ((taskPtr->ownedMutexList.size != 0U) ||
        (taskPtr->waitingForMutexPtr != NULL))
    {
        RK_CR_EXIT
        return (RK_ERR_TASK_INVALID_ST);
    }
#endif

    if (kTaskHasDependents_(taskPtr) == RK_TRUE)
    {
        RK_CR_EXIT
        return (RK_ERR_TASK_INVALID_ST);
    }

    /* if task is running, defer termination to the post-processing sys task */
    if (taskPtr->status == RK_RUNNING)
    {
        if (taskPtr != RK_gRunPtr)
        {
            RK_CR_EXIT
            return (RK_ERR_TASK_INVALID_ST);
        }

        RK_ERR deferErr = kPostProcJobEnq(RK_POSTPROC_JOB_TASK_TERMINATE,
                                          (VOID *)taskPtr, 0U);
        if (deferErr != RK_ERR_SUCCESS)
        {

            K_PANIC("Failed to defer task termination to\
                post-processing system task");

            RK_CR_EXIT

            return (deferErr);
        }

        if (taskHandlePtr != NULL)
        {
            *taskHandlePtr = NULL;
        }

        taskPtr->status = RK_PENDING; /* pending deferred termination */
        kPendCtxSwtch();
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    switch (taskPtr->status)
    {
        case RK_READY:
            if ((taskPtr->tcbNode.nextPtr != NULL) &&
                (taskPtr->tcbNode.prevPtr != NULL))
            {
                RK_TCB *remPtr = taskPtr;
                kTCBQRem(&RK_gReadyQueue[taskPtr->priority], &remPtr);
            }
            break;

        case RK_SLEEPING_DELAY:
        case RK_SLEEPING_RELEASE:
        case RK_SLEEPING_UNTIL:
        case RK_SLEEPING_EV_FLAG:
            if (kTimeoutNodeIsArmed(&taskPtr->timeoutNode) == RK_TRUE)
            {
                err = kRemoveTimeoutNode(&taskPtr->timeoutNode);
                if (err != RK_ERR_SUCCESS)
                {
                    RK_CR_EXIT
                    return (err);
                }
            }
            break;

        case RK_PENDING: /* deferred self-termination path */
            break;

        default:
            RK_CR_EXIT
            return (RK_ERR_TASK_INVALID_ST);
    }

#if (RK_CONF_MESG_QUEUE == ON)
    taskPtr->mesgQueueRecvBufPtr = NULL;
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
    taskPtr->asynchMesgInit = RK_FALSE;
    kListInit(&taskPtr->asynchMesgQueue);
    kListInit(&taskPtr->asynchMesgWaiters);
    kListInit(&taskPtr->asynchMesgOwnedList);
    taskPtr->asynchMesgWaitSenderPtr = NULL;
    taskPtr->asynchMesgWaitDestPtr = NULL;
    taskPtr->asynchMesgAllocDestPtr = NULL;
    taskPtr->asynchMesgWaitStatus = RK_ERR_SUCCESS;
#if (RK_CONF_ASYNCH_COPY_MESG == ON)
    taskPtr->asynchCopyMesgInit = RK_FALSE;
    kListInit(&taskPtr->asynchCopyMesgQueue);
    kListInit(&taskPtr->asynchCopyMesgWaiters);
    taskPtr->asynchCopyMesgWaitSenderPtr = NULL;
    taskPtr->asynchCopyMesgRecvBufPtr = NULL;
    taskPtr->asynchCopyMesgRecvBufBytes = 0UL;
    taskPtr->asynchCopyMesgRecvBytesPtr = NULL;
    taskPtr->asynchCopyMesgRecvStatus = RK_ERR_SUCCESS;
#endif
#endif

#if (RK_CONF_SYNCH_MESG == ON)
    taskPtr->synchMesgMaxBytes = 0UL;
    taskPtr->synchMesgPendingPtr = NULL;
    taskPtr->synchMesgPendingSenderPtr = NULL;
    taskPtr->synchMesgRecvBufPtr = NULL;
    taskPtr->synchMesgRecvBytesPtr = NULL;
    taskPtr->synchMesgRecvStatus = RK_ERR_SUCCESS;
    kListInit(&taskPtr->synchMesgSenders);
    taskPtr->synchMesgPtr = NULL;
    taskPtr->synchMesgBytes = 0UL;
    taskPtr->synchMesgStatus = RK_ERR_SUCCESS;
    taskPtr->synchMesgReceiverPtr = NULL;
    kListInit(&taskPtr->synchMesgCallers);
    kListInit(&taskPtr->synchMesgAcceptWaiters);
    taskPtr->synchMesgActiveCallerPtr = NULL;
    taskPtr->synchMesgActiveCallerPrio = taskPtr->prioNominal;
    taskPtr->synchMesgCallReplyBufPtr = NULL;
    taskPtr->synchMesgCallReplyBytesPtr = NULL;
    taskPtr->synchMesgCallReplyMaxBytes = 0UL;
    taskPtr->synchMesgCallState = RK_SYNCH_CALL_IDLE;
#endif

#if (RK_CONF_MRM == ON)
    err = kMRMTaskCleanup(taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
#endif


    RK_TID const slotPid = taskPid;
    RK_STACK *stackBufPtr = NULL;
    RK_MEM_PARTITION *stackMemPtr = NULL;
    if (slotPid < RK_NTHREADS)
    {
        stackMemPtr = RK_gTaskDynStackPartByPid[slotPid];
        if (stackMemPtr != NULL)
        {
            stackBufPtr = taskPtr->stackBufPtr;
        }
    }

    kMpuTaskMemoryRelease(taskPtr);

    if (taskHandlePtr != NULL)
    {
        *taskHandlePtr = NULL;
    }
    RK_gTaskDynStackPartByPid[slotPid] = NULL;
    kTaskRegistryInvalidate_(taskPtr);

    taskPtr->status = RK_TASK_TERMINATED;
    taskPtr->init = RK_FALSE;

    if ((stackBufPtr != NULL) && (stackMemPtr != NULL))
    {
        err = kMemPartitionFree(stackMemPtr, stackBufPtr);
        if (err != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (err);
        }
    }

    RK_MEMSET(taskPtr, 0, sizeof(RK_TCB));
    err = kMemPartitionFree(&RK_gTaskPool, taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    if (pPid > 0U)
    {
        pPid -= 1U;
    }
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kTaskTerminateTcb(RK_TCB *const taskPtr)
{
    return (kTaskTerminateResolved_(taskPtr, NULL));
}

RK_ERR kTaskTerminate(RK_TASK_HANDLE *taskHandlePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TASK_TERMINATE,
                                        (ULONG)(UINTPTR)taskHandlePtr,
                                        0UL, 0UL, 0UL));
    }

    if ((taskHandlePtr == NULL) || (*taskHandlePtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR err = kTaskHandleResolve(*taskHandlePtr, &taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        kErrHandler((RK_FAULT)err);
#endif
        return (err);
    }

    return (kTaskTerminateResolved_(taskPtr, taskHandlePtr));
}
#endif

RK_TASK_HANDLE kTaskGetRunningHandle(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_TASK_HANDLE)(UINTPTR)kSyscallInvoke4(
            RK_SYSCALL_TASK_GET_RUNNING_HANDLE, 0UL, 0UL, 0UL, 0UL));
    }

    return (kTaskHandleFromTcb(RK_gRunPtr));
}

RK_TID kTaskGetID(RK_TASK_HANDLE taskHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_TID)kSyscallInvoke4(RK_SYSCALL_TASK_GET_ID,
                                        (ULONG)(UINTPTR)taskHandle,
                                        0UL, 0UL, 0UL));
    }

    RK_TCB *taskPtr = NULL;
    if (kTaskHandleResolve(taskHandle, &taskPtr) != RK_ERR_SUCCESS)
    {
        return ((RK_TID)UINT8_MAX);
    }

    return (taskPtr->tid);
}

const CHAR *kTaskGetRunningName(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((const CHAR *)(UINTPTR)kSyscallInvoke4(
            RK_SYSCALL_TASK_GET_RUNNING_NAME, 0UL, 0UL, 0UL, 0UL));
    }

    if (RK_gSyscallThreadModeActive != 0U)
    {
        return (kTaskPublicNameSnapshot_(RK_gRunPtr));
    }

    return ((RK_gRunPtr != NULL) ? RK_gRunPtr->taskName : NULL);
}

RK_ERR kTaskGetName(RK_TASK_HANDLE taskHandle, CHAR *buf)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_TASK_GET_NAME, (ULONG)(UINTPTR)taskHandle,
            (ULONG)(UINTPTR)buf, 0UL, 0UL));
    }

    if (buf == NULL || taskHandle == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    RK_TCB *taskPtr = NULL;
    RK_ERR err = kTaskHandleResolve(taskHandle, &taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    UINT i = 0;
    CHAR *name = &taskPtr->taskName[0];
    while (*name != '\0')
    {
        *buf++ = *name++;
        i++;
        if (i >= RK_OBJ_MAX_NAME_LEN)
        {
            break;
        }
    }
    return (RK_ERR_SUCCESS);
}

RK_PRIO kTaskGetPrio(RK_TASK_HANDLE taskHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_PRIO)kSyscallInvoke4(RK_SYSCALL_TASK_GET_PRIO,
                                         (ULONG)(UINTPTR)taskHandle,
                                         0UL, 0UL, 0UL));
    }

    RK_TCB *taskPtr = NULL;
    if (kTaskHandleResolve(taskHandle, &taskPtr) != RK_ERR_SUCCESS)
    {
        return (RK_PRIO_TYPE_MAX);
    }

    return (taskPtr->priority);
}

RK_PRIO kTaskGetNomPrio(RK_TASK_HANDLE taskHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_PRIO)kSyscallInvoke4(RK_SYSCALL_TASK_GET_NOM_PRIO,
                                         (ULONG)(UINTPTR)taskHandle,
                                         0UL, 0UL, 0UL));
    }

    RK_TCB *taskPtr = NULL;
    if (kTaskHandleResolve(taskHandle, &taskPtr) != RK_ERR_SUCCESS)
    {
        return (RK_PRIO_TYPE_MAX);
    }

    return (taskPtr->prioNominal);
}

/******************************************************************************/
/* KERNEL INITIALISATION                                                      */
/******************************************************************************/
static VOID kInitRunTime_(VOID)
{
    RK_gRunTime.globalTick = 0UL;
    RK_gRunTime.nWraps = 0UL;
}
static RK_ERR kInitQueues_(VOID)
{
    RK_gReadyBitmask = 0UL;
    RK_gReadyPos = 0UL;

    for (ULONG i = 0UL; i < RK_RDYQSIZ; i++)
    {
        RK_ERR err = kTCBQInit(&RK_gReadyQueue[i]);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
    }
    return (RK_ERR_SUCCESS);
}

extern VOID kApplicationInit(VOID);

RK_FUNC_WEAK
VOID kApplicationInit(VOID)
{
    K_ERR_HANDLER(RK_FAULT_MISSING_APPLICATION_INIT);
    K_PANIC("MISSING APPLICATION INIT");
}

VOID kInit(VOID)
{
    RK_gKernelPhase = RK_KERNEL_PHASE_BOOT;
    kFaultTraceInit();

    if (!kIsValidVersion())
    {
        K_PANIC("ERROR: INVALID KERNEL VERSION");
    }

    if (kInitQueues_() != RK_ERR_SUCCESS)
    {
        K_PANIC("FAILED TO INITIALISE READY QUEUES");
    }

    if (kTaskPoolEnsureInit_(RK_NTHREADS) != RK_ERR_SUCCESS)
    {
        K_PANIC("FAILED TO INITIALISE TASK POOL");
    }

    if (kObjPartitionsInit() != RK_ERR_SUCCESS)
    {
        K_PANIC("FAILED TO INITIALISE OBJECT POOLS");
    }

    kApplicationInit();

    {
        RK_ERR const err = kMpuLayoutFinalize();
        if (err != RK_ERR_SUCCESS)
        {
            K_PANIC("FAILED TO FINALISE MPU LAYOUT: %d", err);
        }
    }

    RK_DSB

    if ((RK_gSystemTasksInit == RK_FALSE) || (RK_gIdleTaskHandle == NULL) ||
        (RK_gPostProcTaskHandle == NULL))
    {
        K_PANIC("SYSTEM TASKS NOT INITIALISED");
    }

    RK_ISB

    kInitRunTime_();
    highestPrio = idleTaskPrio;

    for (volatile ULONG i = 0; i < RK_NTHREADS; i++)
    {
        RK_TCB *const taskPtr = RK_gTaskHandleByPid[i];

        if ((taskPtr != NULL) && (taskPtr->init == RK_TRUE) &&
            (taskPtr->status == RK_TCB_INITIALISED) &&
            (taskPtr->priority < highestPrio))
        {
            highestPrio = taskPtr->priority;
        }
    }

    for (volatile ULONG i = 0; i < RK_NTHREADS; i++)
    {
        RK_TCB *const taskPtr = RK_gTaskHandleByPid[i];

        if ((taskPtr != NULL) && (taskPtr->init == RK_TRUE) &&
            (taskPtr->status == RK_TCB_INITIALISED))
        {
            RK_ERR err = kTCBQEnq(&RK_gReadyQueue[taskPtr->priority], taskPtr);
            if (err == RK_ERR_SUCCESS)
            {
                taskPtr->status = RK_READY;
            }
        }
    }

    kTCBQDeq(&RK_gReadyQueue[highestPrio], &RK_gRunPtr);
    RK_gSchLock = RK_gRunPtr->schLock;
    RK_TCB const *const idlePtr = RK_gTaskHandleByPid[RK_IDLETASK_ID];
    if ((RK_gIdleTaskHandle == NULL) || (idlePtr == NULL) ||
        (idlePtr->priority != lowestPrio + 1))
    {
        K_PANIC("INVALID PRIORITY: %s : %d\r\n", RK_gRunPtr->taskName,
                RK_gRunPtr->priority);
    }
    if (RK_gSysTickInterval == 0)
    {
        K_PANIC("INVALID SYSTICK INTERVAL: %ld\r\n", RK_gSysTickInterval);
    }
    RK_DSB
    RK_gStartupSvcArmed = 1U;
    RK_EN_IRQ
    RK_ISB
    /* calls low-level scheduler for start-up */
    RK_STUP
}

/******************************************************************************/
/* TASK SWITCHING LOGIC                                                       */
/******************************************************************************/
static inline RK_PRIO kCalcNextTaskPrio_()
{

    if (RK_gReadyBitmask == 0U)
    {
        return (idleTaskPrio);
    }

    /*
     * Isolate the least significant set bit: priorities are encoded highest to
     * lowest starting at zero, so this finds the highest-priority non-empty
     * ready queue without scanning every priority.
     */
    RK_gReadyPos = RK_gReadyBitmask & -RK_gReadyBitmask;
    volatile RK_PRIO prio = (RK_PRIO)(__getReadyPrio(RK_gReadyPos));
    return (prio);
}

VOID kSwtch(VOID)
{
    RK_TCB *currRK_gRunPtr = RK_gRunPtr;
    RK_TCB *nextRK_gRunPtr = NULL;

    if (RK_gRunPtr->status == RK_RUNNING)
    {
        kPreemptRunningTask_();
    }
    nextTaskPrio = kCalcNextTaskPrio_();

    kTCBQDeq(&RK_gReadyQueue[nextTaskPrio], &nextRK_gRunPtr);

    if (nextRK_gRunPtr == NULL)
    {
        K_PANIC("NULL READY TASK POINTER\r\n");
    }
    currRK_gRunPtr->schLock = RK_gSchLock;
    RK_gRunPtr = nextRK_gRunPtr;
    RK_gSchLock = RK_gRunPtr->schLock;
}
static inline VOID kPreemptRunningTask_(VOID)
{
    kTCBQJam(&RK_gReadyQueue[RK_gRunPtr->priority], RK_gRunPtr);
    RK_BARRIER
    RK_gRunPtr->status = RK_READY;
}

static inline VOID kYieldRunningTask_(VOID)
{
    if (kCalcNextTaskPrio_() <= RK_gRunPtr->priority)
    {
        kTCBQEnq(&RK_gReadyQueue[RK_gRunPtr->priority], RK_gRunPtr);
        RK_gRunPtr->status = RK_READY;
        if (RK_gSchLock == 0U)
        {
            kPendCtxSwtchNow_();
        }
        else
        {
            kDeferCtxSwtch_();
        }
    }
}
/******************************************************************************/
/* TICK MANAGEMENT                                                            */
/******************************************************************************/

/* called from SysTick_Handler */

volatile RK_TIMEOUT_NODE *RK_gTimeOutListHeadPtr = NULL;
volatile RK_TIMEOUT_NODE *RK_gTimerListHeadPtr = NULL;
volatile UINT RK_gSyscallPreemptPending = 0U;
#if (RK_CONF_SVC_DEFER_TEST == ON)
static volatile ULONG RK_gDeferredSysTickBurst = 0UL;
volatile ULONG RK_gDeferredSysTickTotal RK_SECTION_SHARED_RAM = 0UL;
volatile ULONG RK_gDeferredSysTickLastDrain RK_SECTION_SHARED_RAM = 0UL;
volatile ULONG RK_gDeferredSysTickMaxDrain RK_SECTION_SHARED_RAM = 0UL;
volatile ULONG RK_gDeferredSysTickDrainCount RK_SECTION_SHARED_RAM = 0UL;
#endif

static RK_BOOL kSyscallPreemptNeeded_(VOID)
{
    if ((RK_gRunPtr == NULL) || (RK_gRunPtr->status != RK_RUNNING) ||
        (RK_gRunPtr->preempt == RK_NO_PREEMPT) || (RK_gSchLock != 0U))
    {
        return (RK_FALSE);
    }

    return ((kCalcNextTaskPrio_() < RK_gRunPtr->priority) ? RK_TRUE
                                                          : RK_FALSE);
}

static UINT kTickHandlerRun_(VOID)
{
    volatile UINT timeOutTask = RK_FALSE;
    RK_CR_AREA
    RK_CR_ENTER
    RK_gRunTime.globalTick += 1UL;
    kTraceTick();
    if (RK_gRunTime.globalTick == RK_TICK_TYPE_MAX)
    {
        RK_gRunTime.globalTick = 0UL;
        RK_gRunTime.nWraps += 1UL;
    }
    RK_CR_EXIT
    /* handle time out and sleeping list */
    /* the list is not empty, decrement only the head  */
    if (RK_gTimeOutListHeadPtr != NULL)
    {
        RK_CR_ENTER

        timeOutTask = kHandleTimeoutList();

        RK_CR_EXIT
    }

#if (RK_CONF_CALLOUT_TIMER == ON)
    if (RK_gTimerListHeadPtr != NULL)
    {
        RK_CR_ENTER

        if (RK_gTimerListHeadPtr->dtick > 0UL)
        {
            --RK_gTimerListHeadPtr->dtick;
        }

        if (RK_gTimerListHeadPtr->dtick == 0UL)
        {
            kEventSet(RK_gPostProcTaskHandle, RK_POSTPROC_TIMER_SIG);
            timeOutTask = RK_TRUE;
        }

        RK_CR_EXIT
    }
#endif
    if ((RK_gRunPtr->status != RK_READY) && (timeOutTask == RK_FALSE))
    {
        return (0U);
    }

    if (RK_gRunPtr->status == RK_RUNNING)
    {
        if (RK_gRunPtr->preempt == RK_NO_PREEMPT)
        {
            return (0U);
        }
        if (RK_gSchLock > 0UL)
        {
            kDeferCtxSwtch_();
            return (0U);
        }
    }

    if ((RK_gRunPtr->status == RK_READY) && (RK_gSchLock > 0UL))
    {
        kDeferCtxSwtch_();
        return (0U);
    }

    RK_gPendingCtxtSwtch = 0U;
    RK_BARRIER
    return (1U);
}

UINT kTickHandler(VOID)
{
    if (RK_gSyscallThreadModeActive != 0U)
    {
        UINT const dispatchNeeded = kTickHandlerRun_();
#if (RK_CONF_SVC_DEFER_TEST == ON)
        ++RK_gDeferredSysTickBurst;
        ++RK_gDeferredSysTickTotal;
#endif
        if ((dispatchNeeded != 0U) && (kSyscallPreemptNeeded_() == RK_TRUE))
        {
            RK_gSyscallPreemptPending = 1U;
            RK_BARRIER
        }
        return (dispatchNeeded);
    }

    return (kTickHandlerRun_());
}

RK_BOOL kSyscallPreemptPending(VOID)
{
    return ((RK_gSyscallPreemptPending != 0U) ? RK_TRUE : RK_FALSE);
}

VOID kTickDrainDeferredOnSyscallExit(VOID)
{
    RK_CR_AREA

    RK_CR_ENTER
#if (RK_CONF_SVC_DEFER_TEST == ON)
    RK_gDeferredSysTickLastDrain = RK_gDeferredSysTickBurst;
    if (RK_gDeferredSysTickBurst != 0UL)
    {
        ++RK_gDeferredSysTickDrainCount;
        if (RK_gDeferredSysTickBurst > RK_gDeferredSysTickMaxDrain)
        {
            RK_gDeferredSysTickMaxDrain = RK_gDeferredSysTickBurst;
        }
        RK_gDeferredSysTickBurst = 0UL;
    }
#endif
    if (RK_gSyscallThreadModeActive > 0U)
    {
        --RK_gSyscallThreadModeActive;
    }
    RK_gSyscallPreemptPending = 0U;
    RK_CR_EXIT
}
