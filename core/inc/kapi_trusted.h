/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
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
