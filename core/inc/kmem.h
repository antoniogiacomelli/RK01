/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_MEM_H
#define RK_MEM_H
#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kobjs.h>
#ifdef __cplusplus
extern "C" {
#endif

RK_ERR kMemPartitionInit(RK_MEM_PARTITION* const, VOID*, ULONG const, ULONG);
RK_ERR kMemPartitionInitGlobalScope(RK_MEM_PARTITION *const, VOID *,
                                    ULONG const, ULONG);
RK_ERR kMemPartitionInitDomainScope(RK_MEM_PARTITION *const, VOID *,
                                    ULONG const, ULONG, RK_DOMAIN *const);
VOID* kMemPartitionAlloc(RK_MEM_PARTITION* const);
RK_ERR kMemPartitionFree(RK_MEM_PARTITION* const, VOID*);

#ifdef __cplusplus
}
#endif

#endif
