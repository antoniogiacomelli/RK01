/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_API_TRUSTED_H
#define RK_API_TRUSTED_H

#include <kapi_domain.h>

#if (RK_CONF_TRACE == ON)
#include <ktrace.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

RK_ERR kTaskInitPrivileged(RK_TASK_HANDLE *taskHandlePtr,
                           const RK_TASKENTRY taskFunc,
                           VOID *argsPtr,
                           RK_STRING taskName,
                           RK_STACK *const stackBufPtr,
                           const ULONG stackSize,
                           const RK_PRIO priority,
                           const RK_OPTION preempt);
RK_ERR kTaskInitProtected(RK_TCB *const taskPtr,
                          RK_TASKENTRY const taskFunc,
                          VOID *const argsPtr,
                          RK_PRIO const priority,
                          RK_TASK_MEMORY const *const memoryPtr);
RK_ERR kSharedRegionInit(RK_SHARED_REGION *const regionPtr,
                         BYTE *const regionBasePtr,
                         ULONG const regionBytes);
RK_ERR kDomainMapSharedRegion(RK_DOMAIN *const domainPtr,
                              RK_SHARED_REGION *const regionPtr);

#if (RK_CONF_MUTEX == ON)
RK_ERR kMutexCreateGlobalScope(RK_MUTEX_HANDLE *const mutexHandlePtr,
                               RK_STRING objName,
                               UINT protocol);
RK_ERR kMutexCreateDomainScope(RK_MUTEX_HANDLE *const mutexHandlePtr,
                               RK_STRING objName,
                               UINT protocol,
                               RK_DOMAIN *const domainPtr);
#endif

#if (RK_CONF_SEMAPHORE == ON)
RK_ERR kSemaphoreCreateGlobalScope(RK_SEMAPHORE_HANDLE *const semaphoreHandlePtr,
                                   RK_STRING objName,
                                   UINT const initialValue,
                                   UINT const maxValue);
RK_ERR kSemaphoreCreateDomainScope(RK_SEMAPHORE_HANDLE *const semaphoreHandlePtr,
                                   RK_STRING objName,
                                   UINT const initialValue,
                                   UINT const maxValue,
                                   RK_DOMAIN *const domainPtr);
#endif

#if (RK_CONF_SLEEP_QUEUE == ON)
RK_ERR kSleepQueueCreateGlobalScope(RK_SLEEP_QUEUE_HANDLE *const sleepqHandlePtr,
                                    RK_STRING objName);
RK_ERR kSleepQueueCreateDomainScope(RK_SLEEP_QUEUE_HANDLE *const sleepqHandlePtr,
                                    RK_STRING objName,
                                    RK_DOMAIN *const domainPtr);
#endif

#if (RK_CONF_MESG_QUEUE == ON)
RK_ERR kMesgQueueCreateGlobalScope(RK_MESG_QUEUE_HANDLE *const queueHandlePtr,
                                   RK_STRING objName,
                                   VOID *const queueBufPtr,
                                   ULONG const mesgSizeWords,
                                   ULONG const depth);
RK_ERR kMesgQueueCreateDomainScope(RK_MESG_QUEUE_HANDLE *const queueHandlePtr,
                                   RK_STRING objName,
                                   VOID *const queueBufPtr,
                                   ULONG const mesgSizeWords,
                                   ULONG const depth,
                                   RK_DOMAIN *const domainPtr);
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
RK_ERR kMesgPoolInitGlobalScope(RK_MEM_PARTITION *const poolPtr,
                                VOID *const memPoolPtr,
                                ULONG const payloadBytes,
                                ULONG const nMesg,
                                RK_PRIO const ceilingPrio);
RK_ERR kMesgPoolInitDomainScope(RK_MEM_PARTITION *const poolPtr,
                                VOID *const memPoolPtr,
                                ULONG const payloadBytes,
                                ULONG const nMesg,
                                RK_PRIO const ceilingPrio,
                                RK_DOMAIN *const domainPtr);
#endif

#if (RK_CONF_MRM == ON)
RK_ERR kMRMCreateDomainScope(RK_MRM_HANDLE *const mrmHandlePtr,
                             RK_STRING objName,
                             RK_MRM_BUF *const mrmPoolPtr,
                             VOID *mesgPoolPtr,
                             ULONG const nBufs,
                             ULONG const dataSizeWords,
                             RK_DOMAIN *const domainPtr);
#endif

#if (RK_CONF_CALLOUT_TIMER == ON)
RK_ERR kTimerCreateGlobalScope(RK_TIMER_HANDLE *const timerHandlePtr,
                               RK_STRING objName,
                               RK_TICK const delay,
                               RK_TICK const period,
                               RK_TIMER_CALLOUT const callout,
                               VOID *const argsPtr,
                               RK_OPTION const opt);
RK_ERR kTimerCreateDomainScope(RK_TIMER_HANDLE *const timerHandlePtr,
                               RK_STRING objName,
                               RK_TICK const delay,
                               RK_TICK const period,
                               RK_TIMER_CALLOUT const callout,
                               VOID *const argsPtr,
                               RK_OPTION const opt,
                               RK_DOMAIN *const domainPtr);
#endif

RK_ERR kMemPartitionInitGlobalScope(RK_MEM_PARTITION *const kobj,
                                    VOID *memPoolPtr,
                                    ULONG blkSize,
                                    const ULONG numBlocks);
RK_ERR kMemPartitionInitDomainScope(RK_MEM_PARTITION *const kobj,
                                    VOID *memPoolPtr,
                                    ULONG blkSize,
                                    const ULONG numBlocks,
                                    RK_DOMAIN *const domainPtr);

#if (RK_CONF_SYSMON == ON)
VOID kSysMonObjectRegister(RK_KOBJ *const objPtr, RK_ID const objID);
VOID kSysMonObjectUnregister(RK_KOBJ *const objPtr);
#endif

#if (RK_CONF_TRACE == ON)
VOID kTraceRecordObject(VOID *const objPtr,
                        RK_TRACE_OP const op,
                        RK_ERR const err,
                        ULONG const value);
VOID kTraceRecordTaskPrio(RK_TASK_HANDLE const taskHandle,
                          RK_PRIO const oldPrio,
                          RK_PRIO const newPrio);
VOID kTraceRecordTaskOverrun(RK_TRACE_OVERRUN_KIND const kind,
                             RK_TICK const tick,
                             RK_TASK_HANDLE const taskHandle);
VOID kTraceRegisterObject(VOID *const objPtr, RK_ID const objID);
VOID kTraceUnregisterObject(VOID *const objPtr);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RK_API_TRUSTED_H */
