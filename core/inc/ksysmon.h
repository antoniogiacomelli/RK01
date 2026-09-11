/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_SYSMON_H
#define RK_SYSMON_H

#include <kcommondefs.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (RK_CONF_SYSMON == ON)

RK_ERR kSysMonInit(VOID);
VOID kSysMonPoll(VOID);
RK_ERR kSysMonCommand(CHAR const *linePtr, ULONG lineBytes);
RK_ERR kSysMonObjectNameSet(RK_HANDLE const objHandle,
                            CHAR const *const namePtr);
VOID kSysMonObjectRegister(RK_KOBJ *const objPtr, RK_ID const objID);
VOID kSysMonObjectUnregister(RK_KOBJ *const objPtr);

#else

#define RK_SYSMON_INLINE_ RK_FORCE_INLINE static inline

RK_SYSMON_INLINE_ RK_ERR kSysMonInit(VOID)
{
    return (RK_ERR_SUCCESS);
}

RK_SYSMON_INLINE_ VOID kSysMonPoll(VOID)
{
}

RK_SYSMON_INLINE_ RK_ERR kSysMonCommand(CHAR const *linePtr,
                                        ULONG lineBytes)
{
    K_UNUSE(linePtr);
    K_UNUSE(lineBytes);
    return (RK_ERR_SUCCESS);
}

RK_SYSMON_INLINE_ RK_ERR
kSysMonObjectNameSet(RK_HANDLE const objHandle, CHAR const *const namePtr)
{
    K_UNUSE(objHandle);
    K_UNUSE(namePtr);
    return (RK_ERR_SUCCESS);
}

RK_SYSMON_INLINE_ VOID kSysMonObjectRegister(RK_KOBJ *const objPtr,
                                             RK_ID const objID)
{
    K_UNUSE(objPtr);
    K_UNUSE(objID);
}

RK_SYSMON_INLINE_ VOID kSysMonObjectUnregister(RK_KOBJ *const objPtr)
{
    K_UNUSE(objPtr);
}

#undef RK_SYSMON_INLINE_

#endif

#ifndef kObjectNameSet
#define kObjectNameSet(OBJ_HANDLE, NAME_PTR)                                   \
    kSysMonObjectNameSet((RK_HANDLE)(UINTPTR)(OBJ_HANDLE), (NAME_PTR))
#endif

#ifdef __cplusplus
}
#endif

#endif /* RK_SYSMON_H */
