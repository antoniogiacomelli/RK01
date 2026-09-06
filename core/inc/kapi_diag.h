/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_API_DIAG_H
#define RK_API_DIAG_H

#include <kapi_app.h>

#if (RK_CONF_TRACE == ON)
#include <ktrace.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if (RK_CONF_SYSMON == ON)
RK_ERR kSysMonInit(VOID);
VOID kSysMonPoll(VOID);
RK_ERR kSysMonCommand(CHAR const *linePtr, ULONG lineBytes);
RK_ERR kSysMonObjectNameSet(RK_HANDLE const objHandle,
                            CHAR const *const namePtr);
#else
RK_FORCE_INLINE
static inline RK_ERR kSysMonInit(VOID)
{
    return (RK_ERR_SUCCESS);
}

RK_FORCE_INLINE
static inline VOID kSysMonPoll(VOID)
{
}

RK_FORCE_INLINE
static inline RK_ERR kSysMonCommand(CHAR const *linePtr, ULONG lineBytes)
{
    K_UNUSE(linePtr);
    K_UNUSE(lineBytes);
    return (RK_ERR_SUCCESS);
}

RK_FORCE_INLINE
static inline RK_ERR
kSysMonObjectNameSet(RK_HANDLE const objHandle, CHAR const *const namePtr)
{
    K_UNUSE(objHandle);
    K_UNUSE(namePtr);
    return (RK_ERR_SUCCESS);
}
#endif

#ifndef kObjectNameSet
#define kObjectNameSet(OBJ_HANDLE, NAME_PTR)                                  \
    kSysMonObjectNameSet((RK_HANDLE)(UINTPTR)(OBJ_HANDLE), (NAME_PTR))
#endif

#if (RK_CONF_TRACE == ON)
RK_ERR kTraceInit(VOID);
VOID kTracePoll(VOID);
UINT kTraceTaskSnapshot(RK_TRACE_TASK_INFO *const infoPtr,
                        UINT const maxInfo);
UINT kTraceMesgSnapshot(RK_TRACE_OBJECT_INFO *const infoPtr,
                        UINT const maxInfo);
UINT kTraceSemaSnapshot(RK_TRACE_SYNC_INFO *const infoPtr,
                        UINT const maxInfo);
UINT kTraceTimerSnapshot(RK_TRACE_TIMER_INFO *const infoPtr,
                         UINT const maxInfo);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RK_API_DIAG_H */
