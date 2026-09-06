/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_SEMA_H
#define RK_SEMA_H


#ifdef __cplusplus
extern "C" {
#endif

#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kobjs.h>

#if (RK_CONF_SEMAPHORE == ON)
RK_ERR kSemaphoreInit(RK_SEMAPHORE *const, UINT const, UINT const);
RK_ERR kSemaphoreCreate(RK_SEMAPHORE_HANDLE *const, RK_STRING, UINT const,
                        UINT const);
RK_ERR kSemaphoreCreateGlobalScope(RK_SEMAPHORE_HANDLE *const, RK_STRING,
                                   UINT const, UINT const);
RK_ERR kSemaphoreCreateDomainScope(RK_SEMAPHORE_HANDLE *const, RK_STRING,
                                   UINT const, UINT const, RK_DOMAIN *const);
RK_ERR kSemaphoreDestroy(RK_SEMAPHORE_HANDLE *const);
RK_ERR kSemaphorePend(RK_SEMAPHORE_HANDLE const, RK_TICK const);
RK_ERR kSemaphorePost(RK_SEMAPHORE_HANDLE const);
RK_ERR kSemaphoreQuery(RK_SEMAPHORE_HANDLE const, INT *const);
#endif


#ifdef __cplusplus
}
#endif

#endif
