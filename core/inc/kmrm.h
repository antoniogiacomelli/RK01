/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_MRM_H
#define RK_MRM_H

#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kobjs.h>
#include <kstring.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (RK_CONF_MRM == ON)
RK_ERR kMRMInit(RK_MRM *const, RK_MRM_BUF *const, VOID *, ULONG const,
                ULONG const);
RK_ERR kMRMCreate(RK_MRM_HANDLE *const, RK_MRM_BUF *const, VOID *, ULONG const,
                  ULONG const);
RK_ERR kMRMCreateModuleScope(RK_MRM_HANDLE *const, RK_MRM_BUF *const, VOID *,
                             ULONG const, ULONG const, RK_MODULE *const);
RK_ERR kMRMDestroy(RK_MRM_HANDLE *const);
RK_MRM_BUF *kMRMReserve(RK_MRM_HANDLE const);
RK_ERR kMRMPublish(RK_MRM_HANDLE const, RK_MRM_BUF *const, VOID const *);
RK_MRM_BUF *kMRMGet(RK_MRM_HANDLE const, VOID *const);
RK_ERR kMRMUnget(RK_MRM_HANDLE const, RK_MRM_BUF *const);
RK_ERR kMRMTaskCleanup(RK_TCB *const);
RK_BOOL kMRMHasActiveLease(RK_MRM const *const);
#endif

#ifdef __cplusplus
}
#endif

#endif
