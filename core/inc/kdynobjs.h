/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_DYNOBJS_H
#define RK_DYNOBJS_H

#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    RK_DYN_OBJ_TYPE_SEMAPHORE = RK_HANDLE_KIND_SEMAPHORE,
    RK_DYN_OBJ_TYPE_MUTEX = RK_HANDLE_KIND_MUTEX,
    RK_DYN_OBJ_TYPE_SLEEP_QUEUE = RK_HANDLE_KIND_SLEEP_QUEUE,
    RK_DYN_OBJ_TYPE_MESG_QUEUE = RK_HANDLE_KIND_MESG_QUEUE,
    RK_DYN_OBJ_TYPE_TIMER = RK_HANDLE_KIND_TIMER,
    RK_DYN_OBJ_TYPE_MRM = RK_HANDLE_KIND_MRM,
    RK_DYN_OBJ_TYPE_SHARED_MEM = RK_HANDLE_KIND_SHARED_MEM
} RK_DYN_OBJ_TYPE;

RK_ERR kObjPartitionsInit(VOID);
RK_BOOL kDynObjHandleIsEncoded(RK_HANDLE const handle);
RK_ERR kDynObjPublishHandle(RK_DYN_OBJ_TYPE const objType,
                            VOID *const objPtr,
                            RK_HANDLE *const handlePPtr);
RK_ERR kDynObjResolveHandle(RK_DYN_OBJ_TYPE const objType,
                            RK_HANDLE const handle,
                            VOID **const objPPtr);
RK_ERR kDynObjResolveAnyHandle(RK_HANDLE const handle,
                               VOID **const objPPtr);
RK_ERR kDynObjResolveDynamicHandle(RK_DYN_OBJ_TYPE const objType,
                                   RK_HANDLE const handle,
                                   VOID **const objPPtr);
RK_ERR kDynObjInvalidateHandle(RK_DYN_OBJ_TYPE const objType,
                               RK_HANDLE const handle);

#ifdef __cplusplus
}
#endif

#endif
