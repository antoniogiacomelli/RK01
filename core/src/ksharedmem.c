/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * File intent:
 *   Handle-addressed inter-domain shared memory. This service wraps the
 *   low-level shared-region MPU primitive with object lifetime, attachment
 *   accounting and runtime access checks.
 *
 * Contracts/invariants:
 *   - A shared-memory segment maps to one MPU shared region.
 *   - Mapping changes happen only before MPU layout finalisation and before
 *     the target domain has tasks.
 *   - Attachment is domain-scoped. All tasks in an attached domain see the
 *     same address; tasks in the same domain do not need this service.
 *   - Runtime get succeeds only for attached domains and only after at least
 *     two domains have attached the segment.
 */

#define RK_SOURCE_CODE

#include <kdynobjs.h>
#include <kerr.h>
#include <ksch.h>
#include <ksharedmem.h>
#include <kstring.h>
#include <ksyscall.h>
#include <ktrace.h>

static RK_ERR kSharedMemResolve_(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                                 RK_SHARED_MEM **const sharedMemPPtr)
{
    if (sharedMemPPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    *sharedMemPPtr = NULL;

    VOID *objPtr = NULL;
    RK_ERR const err =
        kDynObjResolveHandle(RK_DYN_OBJ_TYPE_SHARED_MEM, sharedMemHandle,
                             &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    *sharedMemPPtr = (RK_SHARED_MEM *)objPtr;
    return (RK_ERR_SUCCESS);
}

static RK_ERR kSharedMemReadyErr_(RK_SHARED_MEM const *const sharedMemPtr)
{
    return (kObjHeaderReadyErr((sharedMemPtr != NULL) ?
                                   &sharedMemPtr->header : NULL,
                               RK_SHARED_MEM_KOBJ_ID));
}

static RK_ERR kSharedMemReportErr_(RK_ERR const err)
{
#if (RK_CONF_ERR_CHECK == ON)
    if (err == RK_ERR_OBJ_NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
    }
    else if (err == RK_ERR_OBJ_NOT_INIT)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
    }
    else if (err == RK_ERR_OBJ_DOUBLE_INIT)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_DOUBLE_INIT);
    }
    else if (err == RK_ERR_INVALID_ISR_PRIMITIVE)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
    }
    else if (err == RK_ERR_INVALID_PHASE)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PHASE);
    }
    else if (err == RK_ERR_INVALID_OBJ)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
    }
    else if (err == RK_ERR_INVALID_PARAM)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
    }
#else
    (VOID)err;
#endif
    return (err);
}

static RK_BOOL kSharedMemAttachedIndex_(RK_SHARED_MEM const *const sharedMemPtr,
                                        RK_DOMAIN const *const domainPtr,
                                        UINT *const idxPtr)
{
    if ((sharedMemPtr == NULL) || (domainPtr == NULL))
    {
        return (RK_FALSE);
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        if (sharedMemPtr->attachedDomainPtr[idx] == domainPtr)
        {
            if (idxPtr != NULL)
            {
                *idxPtr = idx;
            }
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static RK_BOOL kSharedMemFreeAttachSlot_(RK_SHARED_MEM const *const sharedMemPtr,
                                         UINT *const idxPtr)
{
    if ((sharedMemPtr == NULL) || (idxPtr == NULL))
    {
        return (RK_FALSE);
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        if (sharedMemPtr->attachedDomainPtr[idx] == NULL)
        {
            *idxPtr = idx;
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static RK_ERR kSharedMemDomainUnmap_(RK_DOMAIN *const domainPtr,
                                     RK_SHARED_REGION const *const regionPtr)
{
    if ((domainPtr == NULL) || (regionPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (domainPtr->taskCount != 0UL)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    for (UINT idx = 0U; idx < RK_CONF_DOMAIN_SHARED_REGIONS; idx++)
    {
        if (domainPtr->sharedRegionPtr[idx] == regionPtr)
        {
            domainPtr->sharedRegionPtr[idx] = NULL;
            return (RK_ERR_SUCCESS);
        }
    }

    return (RK_ERR_INVALID_PARAM);
}

RK_ERR kSharedMemInit(RK_SHARED_MEM *const sharedMemPtr,
                      VOID *const regionBasePtr,
                      ULONG const regionBytes)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    if ((sharedMemPtr == NULL) || (regionBasePtr == NULL))
    {
        return (kSharedMemReportErr_(RK_ERR_OBJ_NULL));
    }

    if (kIsISR())
    {
        return (kSharedMemReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (kMpuLayoutIsFinalized() == RK_TRUE)
    {
        return (kSharedMemReportErr_(RK_ERR_INVALID_PHASE));
    }

    if (sharedMemPtr->init == RK_TRUE)
    {
        return (kSharedMemReportErr_(RK_ERR_OBJ_DOUBLE_INIT));
    }

    RK_MEMSET(sharedMemPtr, 0, sizeof(*sharedMemPtr));

    RK_ERR err =
        kSharedRegionInit(&sharedMemPtr->region,
                          (BYTE *)(VOID *)regionBasePtr, regionBytes);
    if (err != RK_ERR_SUCCESS)
    {
        RK_MEMSET(sharedMemPtr, 0, sizeof(*sharedMemPtr));
        return (kSharedMemReportErr_(err));
    }

    sharedMemPtr->objID = RK_SHARED_MEM_KOBJ_ID;
    sharedMemPtr->init = RK_TRUE;
    err = kObjHeaderScopeSet(&sharedMemPtr->header, RK_SCOPE_KERNEL_GLOBAL,
                             NULL);
    if (err != RK_ERR_SUCCESS)
    {
        kMpuSharedRegionMemoryRelease(&sharedMemPtr->region);
        RK_MEMSET(sharedMemPtr, 0, sizeof(*sharedMemPtr));
        return (kSharedMemReportErr_(err));
    }
    sharedMemPtr->attachCount = 0UL;
    kTraceRegisterObject(sharedMemPtr, RK_SHARED_MEM_KOBJ_ID);
    return (RK_ERR_SUCCESS);
}

RK_ERR kSharedMemAttach(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                        RK_DOMAIN *const domainPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SHARED_MEM_ATTACH, (ULONG)sharedMemHandle,
            (ULONG)(UINTPTR)domainPtr, 0UL, 0UL));
    }

    if ((sharedMemHandle == RK_NULL_HANDLE) || (domainPtr == NULL))
    {
        return (kSharedMemReportErr_(RK_ERR_OBJ_NULL));
    }

    if (kIsISR())
    {
        return (kSharedMemReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (kMpuLayoutIsFinalized() == RK_TRUE)
    {
        return (kSharedMemReportErr_(RK_ERR_INVALID_PHASE));
    }

    RK_SHARED_MEM *sharedMemPtr = NULL;
    RK_ERR err = kSharedMemResolve_(sharedMemHandle, &sharedMemPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (kSharedMemReportErr_(err));
    }

    err = kSharedMemReadyErr_(sharedMemPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (kSharedMemReportErr_(err));
    }

    if (domainPtr->init != RK_TRUE)
    {
        return (kSharedMemReportErr_(RK_ERR_OBJ_NOT_INIT));
    }

    if (kSharedMemAttachedIndex_(sharedMemPtr, domainPtr, NULL) == RK_TRUE)
    {
        return (kSharedMemReportErr_(RK_ERR_OBJ_DOUBLE_INIT));
    }

    UINT attachIdx = 0U;
    if (kSharedMemFreeAttachSlot_(sharedMemPtr, &attachIdx) != RK_TRUE)
    {
        return (RK_ERR_BUFFER_FULL);
    }

    err = kDomainMapSharedRegion(domainPtr, &sharedMemPtr->region);
    if (err != RK_ERR_SUCCESS)
    {
        return (kSharedMemReportErr_(err));
    }

    sharedMemPtr->attachedDomainPtr[attachIdx] = domainPtr;
    sharedMemPtr->attachCount++;
    kTraceRecordObject(sharedMemPtr, RK_TRACE_OP_ALLOC, RK_ERR_SUCCESS,
                       sharedMemPtr->attachCount);
    return (RK_ERR_SUCCESS);
}

RK_ERR kSharedMemDetach(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                        RK_DOMAIN *const domainPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SHARED_MEM_DETACH, (ULONG)sharedMemHandle,
            (ULONG)(UINTPTR)domainPtr, 0UL, 0UL));
    }

    if ((sharedMemHandle == RK_NULL_HANDLE) || (domainPtr == NULL))
    {
        return (kSharedMemReportErr_(RK_ERR_OBJ_NULL));
    }

    if (kIsISR())
    {
        return (kSharedMemReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    if (kMpuLayoutIsFinalized() == RK_TRUE)
    {
        return (kSharedMemReportErr_(RK_ERR_INVALID_PHASE));
    }

    RK_SHARED_MEM *sharedMemPtr = NULL;
    RK_ERR err = kSharedMemResolve_(sharedMemHandle, &sharedMemPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (kSharedMemReportErr_(err));
    }

    err = kSharedMemReadyErr_(sharedMemPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (kSharedMemReportErr_(err));
    }

    UINT attachIdx = 0U;
    if (kSharedMemAttachedIndex_(sharedMemPtr, domainPtr,
                                 &attachIdx) != RK_TRUE)
    {
        return (kSharedMemReportErr_(RK_ERR_INVALID_PARAM));
    }

    err = kSharedMemDomainUnmap_(domainPtr, &sharedMemPtr->region);
    if (err != RK_ERR_SUCCESS)
    {
        return (kSharedMemReportErr_(err));
    }

    sharedMemPtr->attachedDomainPtr[attachIdx] = NULL;
    if (sharedMemPtr->attachCount > 0UL)
    {
        sharedMemPtr->attachCount--;
    }
    kTraceRecordObject(sharedMemPtr, RK_TRACE_OP_FREE, RK_ERR_SUCCESS,
                       sharedMemPtr->attachCount);
    return (RK_ERR_SUCCESS);
}

RK_ERR kSharedMemGet(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                     VOID **const regionBasePPtr,
                     ULONG *const regionBytesPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SHARED_MEM_GET, (ULONG)sharedMemHandle,
            (ULONG)(UINTPTR)regionBasePPtr,
            (ULONG)(UINTPTR)regionBytesPtr, 0UL));
    }

    if ((sharedMemHandle == RK_NULL_HANDLE) ||
        (regionBasePPtr == NULL) || (regionBytesPtr == NULL))
    {
        return (kSharedMemReportErr_(RK_ERR_OBJ_NULL));
    }

    if (kIsISR())
    {
        return (kSharedMemReportErr_(RK_ERR_INVALID_ISR_PRIMITIVE));
    }

    RK_SHARED_MEM *sharedMemPtr = NULL;
    RK_ERR err = kSharedMemResolve_(sharedMemHandle, &sharedMemPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (kSharedMemReportErr_(err));
    }

    err = kSharedMemReadyErr_(sharedMemPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (kSharedMemReportErr_(err));
    }

    if (sharedMemPtr->attachCount < 2UL)
    {
        return (kSharedMemReportErr_(RK_ERR_INVALID_PARAM));
    }

    if ((RK_gRunPtr != NULL) &&
        (kSharedMemAttachedIndex_(sharedMemPtr, RK_gRunPtr->domainPtr,
                                  NULL) != RK_TRUE))
    {
        return (kSharedMemReportErr_(RK_ERR_INVALID_PARAM));
    }

    *regionBasePPtr = (VOID *)sharedMemPtr->region.regionBasePtr;
    *regionBytesPtr = sharedMemPtr->region.regionBytes;
    return (RK_ERR_SUCCESS);
}
