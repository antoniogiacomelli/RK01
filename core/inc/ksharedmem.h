/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_SHAREDMEM_H
#define RK_SHAREDMEM_H

#include <kenv.h>
#include <kcommondefs.h>
#include <kobjs.h>

#ifdef __cplusplus
extern "C" {
#endif

RK_ERR kSharedMemInit(RK_SHARED_MEM *const sharedMemPtr,
                      VOID *const regionBasePtr,
                      ULONG const regionBytes);
RK_ERR kSharedMemCreate(RK_SHARED_MEM_HANDLE *const sharedMemHandlePtr,
                        RK_STRING objName,
                        VOID *const regionBasePtr,
                        ULONG const regionBytes);
RK_ERR kSharedMemDestroy(RK_SHARED_MEM_HANDLE *const sharedMemHandlePtr);
RK_ERR kSharedMemAttach(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                        RK_DOMAIN *const domainPtr);
RK_ERR kSharedMemDetach(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                        RK_DOMAIN *const domainPtr);
RK_ERR kSharedMemGet(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                     VOID **const regionBasePPtr,
                     ULONG *const regionBytesPtr);

#ifdef __cplusplus
}
#endif

#endif /* RK_SHAREDMEM_H */
