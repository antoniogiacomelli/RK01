/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_SCH_H
#define RK_SCH_H
#ifdef __cplusplus
{
#endif
#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kmpu.h>
#include <kobjs.h>
#include <klist.h>
#include <kstring.h>
/* Globals */

extern RK_TCB* RK_gRunPtr; /* Pointer to the running TCB */
extern RK_TCB RK_gTcbs[RK_NTHREADS]; /* Built-in TCB pool storage */
extern RK_TCB *RK_gTaskHandleByPid[RK_NTHREADS];
extern volatile RK_FAULT RK_gFaultID; /* Fault ID */
extern UINT RK_gIdleStack[RK_CONF_IDLE_STACKSIZE]; /* Stack for idle task */
extern UINT RK_gPostProcStack[RK_CONF_POSTPROC_STACKSIZE];
extern RK_TCBQ RK_gReadyQueue[RK_RDYQSIZ]; /* Table of ready queues */
extern volatile ULONG RK_gReadyBitmask;
extern volatile ULONG RK_gReadyPos;
extern volatile UINT RK_gPendingCtxtSwtch;
extern volatile UINT RK_gSchLock;
extern volatile UINT RK_gStartupSvcArmed;
extern volatile UINT RK_gKernelPhase;
extern volatile UINT RK_gKernelConstructionDepth;
extern volatile UINT RK_gSyscallPreemptPending;
#if (RK_CONF_SVC_DEFER_TEST == ON)
/* Test counters for SysTick interrupts observed while an SVC syscall was
 * active. Timeout servicing is immediate; only task dispatch waits for SVC
 * exception return. */
extern volatile ULONG RK_gDeferredSysTickTotal;
extern volatile ULONG RK_gDeferredSysTickLastDrain;
extern volatile ULONG RK_gDeferredSysTickMaxDrain;
extern volatile ULONG RK_gDeferredSysTickDrainCount;
#endif

/* internal return values */
#ifndef RK_ERR_RESCHED_PENDING
#define RK_ERR_RESCHED_PENDING                ((RK_ERR)900)
#endif
#ifndef RK_ERR_RESCHED_NOT_NEEDED
#define RK_ERR_RESCHED_NOT_NEEDED            ((RK_ERR)901)
#endif
#ifndef RK_CONF_MIN_PRIO
#define RK_CONF_MIN_PRIO 31
#endif

#define RK_KERNEL_PHASE_BOOT (0U)
#define RK_KERNEL_PHASE_RUNNING (1U)

VOID kSchLock(VOID);
VOID kSchUnlock(VOID);
VOID kPendCtxSwtch(VOID);
UINT kTickHandler(VOID);
RK_BOOL kSyscallPreemptPending(VOID);
VOID kTickDrainDeferredOnSyscallExit(VOID);
VOID kSwtch(VOID);
VOID kInit(VOID);
RK_BOOL kKernelBootPhase(VOID);
RK_BOOL kKernelRunning(VOID);
VOID kKernelConstructionEnter(VOID);
VOID kKernelConstructionExit(VOID);
RK_BOOL kKernelRawInitAllowed(VOID);
RK_ERR kKernelRawInitGuard(VOID);
VOID kYield(VOID);
RK_ERR kTaskFaultTerminate(RK_TCB *const taskPtr);
RK_ERR kTaskFaultCleanup(RK_TID const tid);
RK_TASK_HANDLE kTaskHandleFromTcb(RK_TCB const *const taskPtr);
RK_ERR kTaskHandleResolve(RK_TASK_HANDLE const taskHandle,
                          RK_TCB **const taskPPtr);
RK_ERR kTaskHandleResolveOrRunning(RK_TASK_HANDLE const taskHandle,
                                   RK_TCB **const taskPPtr);
RK_ERR kTaskTerminateTcb(RK_TCB *const taskPtr);
#ifndef kPreemptEnable
#define kPreemptEnable kSchUnlock
#endif
#ifndef kPreemptDisable
#define kPreemptDisable kSchLock
#endif

/* Task queue management */
RK_ERR kTCBQInit(RK_TCBQ *const);
RK_ERR kTCBQEnq(RK_TCBQ *const, RK_TCB *const);
RK_ERR kTCBQJam(RK_TCBQ *const, RK_TCB *const);
RK_ERR kTCBQDeq(RK_TCBQ *const, RK_TCB **const);
RK_ERR kTCBQRem(RK_TCBQ *const, RK_TCB **const);
RK_TCB *kTCBQPeek(RK_TCBQ *const);
RK_ERR kTCBQEnqByPrio(RK_TCBQ *const, RK_TCB *const);
RK_ERR kReschedTask(RK_TCB *);
RK_ERR kReschedRunning(VOID);
RK_BOOL kTaskUpdateEffectivePrio(RK_TCB *const);
VOID kTaskUpdateEffectivePrioChain(RK_TCB *const);
RK_ERR kReadySwtch(RK_TCB *const);
RK_ERR kReadyNoSwtch(RK_TCB *const);
RK_ERR kTaskInit(RK_TASK_HANDLE *,
                   const RK_TASKENTRY, VOID *,
                   CHAR *const, RK_STACK *const,
                   const ULONG, const RK_PRIO,
                   const RK_OPTION);
RK_DOMAIN *kApplicationDomainGet(VOID);
RK_ERR kApplicationDomainEnsureInit(VOID);
RK_ERR kDomainInit(RK_DOMAIN *const, BYTE *const, ULONG const, CHAR *const);
VOID *kDomainAlloc(RK_DOMAIN *const, ULONG const, ULONG const);
RK_STACK *kDomainStackAlloc(RK_DOMAIN *const, ULONG const);
RK_ERR kDomainTaskInit(RK_DOMAIN *const,
                       RK_TASK_HANDLE *,
                       const RK_TASKENTRY, VOID *,
                       CHAR *const,
                       const ULONG, const RK_PRIO,
                       const RK_OPTION);
RK_ERR kSharedRegionInit(RK_SHARED_REGION *const, BYTE *const, ULONG const);
RK_ERR kDomainMapSharedRegion(RK_DOMAIN *const, RK_SHARED_REGION *const);
RK_ERR kTaskInitDomain(RK_TASK_HANDLE *,
                       const RK_TASKENTRY, VOID *,
                       CHAR *const, RK_STACK *const,
                       const ULONG, const RK_PRIO,
                       const RK_OPTION, RK_DOMAIN *const);
RK_ERR kTaskInitPrivileged(RK_TASK_HANDLE *,
                           const RK_TASKENTRY, VOID *,
                           CHAR *const, RK_STACK *const,
                           const ULONG, const RK_PRIO,
                           const RK_OPTION);
RK_ERR kTaskInitIsolated(RK_TASK_HANDLE *,
                         const RK_TASKENTRY, VOID *,
                         CHAR *const, RK_STACK *const,
                         const ULONG, const RK_PRIO,
                         const RK_OPTION);
RK_ERR kTaskInitProtected(RK_TCB *const,
                          RK_TASKENTRY const,
                          VOID *const,
                          RK_PRIO const,
                          RK_TASK_MEMORY const *const);
#if (RK_CONF_DYNAMIC_TASK == ON)
RK_ERR kTaskSpawn(RK_DYNAMIC_TASK_ATTR const * ,
                  RK_TASK_HANDLE * );

RK_ERR kTaskTerminate(RK_TASK_HANDLE *);
RK_ERR kTaskTerminateSelf(VOID);
#endif

RK_TASK_HANDLE kTaskGetRunningHandle(VOID);
const CHAR *kTaskGetRunningName(VOID);
RK_TID kTaskGetID(RK_TASK_HANDLE taskHandle);
RK_ERR kTaskGetName(RK_TASK_HANDLE taskHandle, CHAR *buf);
RK_PRIO kTaskGetPrio(RK_TASK_HANDLE taskHandle);
RK_PRIO kTaskGetNomPrio(RK_TASK_HANDLE taskHandle);



#ifdef __cplusplus
}
#endif

#endif /* KSCH_H */
