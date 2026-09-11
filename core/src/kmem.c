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
 *   Fixed-size memory partition allocator. It validates partition/block
 *   membership, keeps allocation bounded and rejects blocks that an
 *   unprivileged caller cannot legally access.
 *
 * Contracts/invariants:
 *   - Every free-list entry is a block-aligned pointer inside the partition.
 *   - nFreeBlocks matches the number of reachable free-list entries.
 *   - Free rejects pointers outside the partition and duplicate free attempts.
 *   - Under MPU, allocation never returns kernel-only memory to an
 *     unprivileged caller.
 */

#define RK_SOURCE_CODE

#include <kmem.h>
#include <ksch.h>
#include <ksyscall.h>
#include <ktrace.h>

/*
 * A partition block is valid only when it is inside the configured pool and
 * exactly aligned to a block boundary. The overflow checks protect the pool-end
 * arithmetic before the membership test.
 */
RK_FORCE_INLINE
static inline RK_BOOL kMemPartitionBlockValid_(
    RK_MEM_PARTITION const *const kobj,
    VOID const *const blockPtr)
{
    if ((kobj == NULL) || (blockPtr == NULL) || (kobj->poolPtr == NULL) ||
        (kobj->blkSize == 0UL) || (kobj->nMaxBlocks == 0UL) ||
        (kobj->blkSize > (RK_ULONG_MAX / kobj->nMaxBlocks)))
    {
        return (RK_FALSE);
    }

    BYTE const *const poolStartPtr = kobj->poolPtr;
    ULONG const poolSize = kobj->blkSize * kobj->nMaxBlocks;
    if ((UINTPTR)poolStartPtr > ((UINTPTR)RK_ULONG_MAX - (UINTPTR)poolSize))
    {
        return (RK_FALSE);
    }

    BYTE const *const poolEndPtr =
        poolStartPtr + poolSize;
    BYTE const *const freeBytePtr = (BYTE const *)blockPtr;

    if ((freeBytePtr < poolStartPtr) || (freeBytePtr >= poolEndPtr))
    {
        return (RK_FALSE);
    }

    ULONG const diff = (ULONG)(freeBytePtr - poolStartPtr);
    return (((diff % kobj->blkSize) == 0UL) ? RK_TRUE : RK_FALSE);
}

/*
 * Validate every free-list next pointer before it is dereferenced. If the list
 * is already corrupt, report "contains" so a free attempt fails closed instead
 * of extending the corruption.
 */
RK_FORCE_INLINE
static inline RK_BOOL kMemPartitionFreeListContains_(
    RK_MEM_PARTITION const *const kobj,
    VOID const *const blockPtr)
{
    if (kobj == NULL)
    {
        return (RK_FALSE);
    }

    BYTE *freeBlockPtr = kobj->freeListPtr;

    for (ULONG i = 0UL; (i < kobj->nFreeBlocks) && (freeBlockPtr != NULL);
         i++)
    {
        if (kMemPartitionBlockValid_(kobj, freeBlockPtr) == RK_FALSE)
        {
            return (RK_TRUE);
        }

        if ((VOID const *)freeBlockPtr == blockPtr)
        {
            return (RK_TRUE);
        }

        freeBlockPtr = *(BYTE **)freeBlockPtr;
    }

    return (RK_FALSE);
}

RK_FORCE_INLINE
static inline RK_ERR kMemPartitionReadyErr_(
    RK_MEM_PARTITION const *const kobj)
{
    RK_ERR const readyErr =
        kObjHeaderReadyErr((kobj != NULL) ? &kobj->header : NULL,
                           RK_MEMALLOC_KOBJ_ID);
    if (readyErr != RK_ERR_SUCCESS)
    {
        return (readyErr);
    }

    return (kObjHeaderDomainLocalAccessErr(
        &kobj->header, (RK_gRunPtr != NULL) ? RK_gRunPtr->domainPtr : NULL));
}

static RK_ERR kMemPartitionReportReadyErr_(RK_ERR const err)
{
#if (RK_CONF_ERR_CHECK == ON)
    if (err == RK_ERR_OBJ_NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
    }
    else if (err == RK_ERR_INVALID_OBJ)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
    }
    else if (err == RK_ERR_OBJ_NOT_INIT)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
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

static RK_ERR kMemPartitionScopeAttrErr_(RK_OBJ_ATTR const *const attrPtr)
{
    if (attrPtr == NULL)
    {
        return (RK_ERR_SUCCESS);
    }

    if (attrPtr->scope == RK_SCOPE_KERNEL_GLOBAL)
    {
        return ((attrPtr->domainPtr == NULL) ? RK_ERR_SUCCESS
                                             : RK_ERR_INVALID_PARAM);
    }

    if (attrPtr->scope == RK_SCOPE_DOMAIN_LOCAL)
    {
        if (attrPtr->domainPtr == NULL)
        {
            return (RK_ERR_INVALID_PARAM);
        }

        return ((attrPtr->domainPtr->init == RK_TRUE) ? RK_ERR_SUCCESS
                                                      : RK_ERR_OBJ_NOT_INIT);
    }

    return (RK_ERR_INVALID_PARAM);
}

static RK_ERR kMemPartitionInitImpl_(RK_MEM_PARTITION *const kobj,
                                     VOID *memPoolPtr,
                                     ULONG blkSize,
                                     ULONG const numBlocks,
                                     RK_OBJ_ATTR const *const attrPtr)
{
    RK_ERR const attrErr = kMemPartitionScopeAttrErr_(attrPtr);
    if (attrErr != RK_ERR_SUCCESS)
    {
        return (attrErr);
    }

    RK_ERR const phaseErr = kKernelRawInitGuard();
    if (phaseErr != RK_ERR_SUCCESS)
    {
        return (phaseErr);
    }

    RK_CR_AREA

    RK_CR_ENTER

    if ((kobj == NULL) || (memPoolPtr == NULL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    if (kobj->init == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_DOUBLE_INIT);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_DOUBLE_INIT);
    }

    if ((blkSize == 0UL) || (numBlocks == 0UL) ||
        (blkSize > (RK_ULONG_MAX - (RK_WORD_SIZE - 1UL))) ||
        (((ULONG)memPoolPtr & (RK_WORD_SIZE - 1UL)) != 0UL))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

    /* rounds up to next multiple of 4*/
    blkSize = ((blkSize + RK_WORD_SIZE - 1) & ~(RK_WORD_SIZE - 1));

    /* initialise freelist of blocks */

    ULONG *blockPtr = (ULONG *)memPoolPtr;
    VOID **nextAddrPtr = (VOID **)memPoolPtr; /* next block address */

    for (ULONG i = 0; i < numBlocks - 1; i++)
    {
        ULONG incSizeWord = blkSize / RK_WORD_SIZE;
        blockPtr += incSizeWord;
        /* save blockPtr addr as the next */
        *nextAddrPtr = (VOID *)blockPtr;
        /* update  */
        nextAddrPtr = (VOID **)(blockPtr);
    }
    *nextAddrPtr = NULL;

    /* init the control block */
    kobj->blkSize = blkSize;
    kobj->nMaxBlocks = numBlocks;
    kobj->nFreeBlocks = numBlocks;
    kobj->freeListPtr = memPoolPtr;
    kobj->poolPtr = memPoolPtr;
    RK_ERR const queueErr = kTCBQInit(&kobj->waitingQueue);
    if (queueErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (queueErr);
    }
#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
    /*
     * A plain memory partition has no message ceiling. kMesgPoolInit()
     * enables these fields only for asynchronous direct-message pools.
     */
    kobj->mesgPrioCeiling = RK_MESG_PRIO_CEILING_NONE;
    kobj->mesgPrioCeilingEnabled = RK_FALSE;
#endif
    kobj->init = RK_TRUE;
    kobj->objID = RK_MEMALLOC_KOBJ_ID;
    kobj->objName[0] = '\0';
    kObjHeaderOwnerDomainSet(&kobj->header,
                             (RK_gRunPtr != NULL) ? RK_gRunPtr->domainPtr
                                                  : NULL);
    if (attrPtr != NULL)
    {
        RK_ERR const scopeErr =
            kObjHeaderScopeSet(&kobj->header, attrPtr->scope,
                               attrPtr->domainPtr);
        if (scopeErr != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (scopeErr);
        }
    }
    kTraceRegisterObject(kobj, RK_MEMALLOC_KOBJ_ID);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kMemPartitionInit(RK_MEM_PARTITION *const kobj, VOID *memPoolPtr,
                         ULONG blkSize, ULONG const numBlocks)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MEM_PARTITION_INIT,
                                        (ULONG)(UINTPTR)kobj,
                                        (ULONG)(UINTPTR)memPoolPtr,
                                        (ULONG)blkSize,
                                        (ULONG)numBlocks));
    }

    RK_OBJ_ATTR attr;
    RK_OBJ_ATTR const *attrPtr = NULL;
    if (RK_gRunPtr == NULL)
    {
        RK_ERR const appDomainErr = kApplicationDomainEnsureInit();
        if (appDomainErr != RK_ERR_SUCCESS)
        {
            return (appDomainErr);
        }

        attr.scope = RK_SCOPE_DOMAIN_LOCAL;
        attr.domainPtr = kApplicationDomainGet();
        attrPtr = &attr;
    }

    return (kMemPartitionInitImpl_(kobj, memPoolPtr, blkSize, numBlocks,
                                   attrPtr));
}

RK_ERR kMemPartitionInitGlobalScope(RK_MEM_PARTITION *const kobj,
                                    VOID *memPoolPtr,
                                    ULONG blkSize,
                                    ULONG const numBlocks)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    RK_OBJ_ATTR const attr = { RK_SCOPE_KERNEL_GLOBAL, NULL };
    return (kMemPartitionInitImpl_(kobj, memPoolPtr, blkSize, numBlocks,
                                   &attr));
}

RK_ERR kMemPartitionInitDomainScope(RK_MEM_PARTITION *const kobj,
                                    VOID *memPoolPtr,
                                    ULONG blkSize,
                                    ULONG const numBlocks,
                                    RK_DOMAIN *const domainPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    RK_OBJ_ATTR const attr = { RK_SCOPE_DOMAIN_LOCAL, domainPtr };
    return (kMemPartitionInitImpl_(kobj, memPoolPtr, blkSize, numBlocks,
                                   &attr));
}

VOID *kMemPartitionAlloc(RK_MEM_PARTITION *const kobj)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((VOID *)(UINTPTR)kSyscallInvoke4(
            RK_SYSCALL_MEM_PARTITION_ALLOC, (ULONG)(UINTPTR)kobj,
            0UL, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMemPartitionReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        kMemPartitionReportReadyErr_(readyErr);
        RK_CR_EXIT
        return (NULL);
    }

    VOID *allocPtr = NULL;

    if (kobj->nFreeBlocks > 0)
    {
        if (kMemPartitionBlockValid_(kobj, kobj->freeListPtr) == RK_FALSE)
        {
#if (RK_CONF_ERR_CHECK == ON)
            K_ERR_HANDLER(RK_FAULT_MEM_FREE);
#endif
            kTraceRecordObject(kobj, RK_TRACE_OP_ALLOC, RK_ERR_MEM_FREE,
                               kobj->nFreeBlocks);
            RK_CR_EXIT
            return (NULL);
        }

        allocPtr = kobj->freeListPtr;
        VOID *const nextPtr = *(VOID **)allocPtr;
        if (((kobj->nFreeBlocks == 1UL) && (nextPtr != NULL)) ||
            ((kobj->nFreeBlocks > 1UL) &&
             (kMemPartitionBlockValid_(kobj, nextPtr) == RK_FALSE)))
        {
#if (RK_CONF_ERR_CHECK == ON)
            K_ERR_HANDLER(RK_FAULT_MEM_FREE);
#endif
            kTraceRecordObject(kobj, RK_TRACE_OP_ALLOC, RK_ERR_MEM_FREE,
                               kobj->nFreeBlocks);
            RK_CR_EXIT
            return (NULL);
        }

        RK_BARRIER
        kobj->nFreeBlocks -= 1;
        kobj->freeListPtr = nextPtr;
        kTraceRecordObject(kobj, RK_TRACE_OP_ALLOC, RK_ERR_SUCCESS,
                           kobj->nFreeBlocks);
    }
    else
    {
        kTraceRecordObject(kobj, RK_TRACE_OP_ALLOC, RK_ERR_BUFFER_EMPTY,
                           kobj->nFreeBlocks);
    }
    RK_CR_EXIT
    return (allocPtr);
}

RK_ERR kMemPartitionFree(RK_MEM_PARTITION *const kobj, VOID *blockPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MEM_PARTITION_FREE, (ULONG)(UINTPTR)kobj,
            (ULONG)(UINTPTR)blockPtr, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_ERR const readyErr = kMemPartitionReadyErr_(kobj);
    if (readyErr != RK_ERR_SUCCESS)
    {
        kMemPartitionReportReadyErr_(readyErr);
        RK_CR_EXIT
        return (readyErr);
    }

    if (blockPtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }

    /* all blocks belonging to this pool are free */
    RK_BOOL allFree = (kobj->nFreeBlocks == kobj->nMaxBlocks);
    if ((kMemPartitionBlockValid_(kobj, blockPtr) == RK_FALSE) || allFree ||
        /* check for double free */
        (kMemPartitionFreeListContains_(kobj, blockPtr) != RK_FALSE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_MEM_FREE);
#endif
        RK_CR_EXIT
        return (RK_ERR_MEM_FREE);
    }

    *(VOID **)blockPtr = kobj->freeListPtr;
    kobj->freeListPtr = blockPtr;
    kobj->nFreeBlocks += 1;
    kTraceRecordObject(kobj, RK_TRACE_OP_FREE, RK_ERR_SUCCESS,
                       kobj->nFreeBlocks);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}
