/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_MUTEX_H
#define RK_MUTEX_H

#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kobjs.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (RK_CONF_MUTEX == ON)
RK_ERR kMutexInit(RK_MUTEX *const, UINT);
RK_ERR kMutexCreate(RK_MUTEX_HANDLE *const, RK_STRING, UINT);
RK_ERR kMutexCreateGlobalScope(RK_MUTEX_HANDLE *const, RK_STRING, UINT);
RK_ERR kMutexCreateDomainScope(RK_MUTEX_HANDLE *const, RK_STRING, UINT,
                               RK_DOMAIN *const);
RK_ERR kMutexDestroy(RK_MUTEX_HANDLE *const);
RK_ERR kMutexLock(RK_MUTEX_HANDLE const, RK_TICK const);
RK_ERR kMutexUnlock(RK_MUTEX_HANDLE const);
RK_ERR kMutexQuery(RK_MUTEX_HANDLE const, UINT *const);
#endif

#ifdef __cplusplus
}
#endif

#endif
