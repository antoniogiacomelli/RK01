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
 *   ARMv8-M MPU policy and validation. This code builds domain/shared-region
 *   maps, loads per-task MPU regions and validates user pointers at the SVC
 *   boundary before privileged code dereferences them.
 *
 * Contracts/invariants:
 *   - User ranges are validated as overflow-safe half-open intervals.
 *   - Domain/shared reservations cannot overlap except by explicit mapping.
 *   - Context switch loads only prevalidated RBAR/RLAR values from the TCB.
 *   - MemManage recovery never trusts stacked PC/LR when the frame is invalid.
 */

#define RK_SOURCE_CODE
#include <kmpu.h>
#include <kcoredefs.h>
#include <kerr.h>
#include <ksch.h>

/*
 * ARMv8-M MPU model
 * -----------------
 * RK01 uses a small, explicit user map for unprivileged tasks:
 *
 * - Region 0 is executable/readable FLASH and is installed once in kMpuInit().
 * - Region 1 is the currently active domain RAM authority window.
 * - Region 2 is the currently running task's private stack.
 * - Region 3 is the shared RAM window used for cross-task handles and other
 *   deliberately shared application state.
 * - Regions 4..7 are optional explicit shared regions mapped only into the
 *   domains that opt into them.
 *
 * Privileged code runs with MPU_CTRL.PRIVDEFENA set, so kernel handlers and
 * privileged system tasks can still use the default privileged memory map.
 * Unprivileged tasks only see regions installed in their TCB.
 */
volatile RK_MPU_FAULT_INFO RK_gMpuFaultInfo = {0};


extern BYTE __rk_flash_begin;
extern BYTE __rk_flash_end;
extern BYTE __rk_task_ram_begin;
extern BYTE __rk_task_ram_end;
extern BYTE __rk_task_stack_begin;
extern BYTE __rk_task_stack_end;
extern BYTE __rk_shared_ram_begin;
extern BYTE __rk_shared_ram_end;

typedef struct RK_STRUCT_MPU_DOMAIN_RESERVATION
{
    RK_DOMAIN *domainPtr;
    BYTE *regionBasePtr;
    ULONG regionBytes;
} RK_MPU_DOMAIN_RESERVATION;

typedef struct RK_STRUCT_MPU_SHARED_RESERVATION
{
    RK_SHARED_REGION *regionPtr;
    BYTE *regionBasePtr;
    ULONG regionBytes;
} RK_MPU_SHARED_RESERVATION;

static RK_TCB *RK_gMpuActiveTaskPtr;
static RK_DOMAIN *RK_gMpuActiveDomainMapPtr;
static volatile RK_BOOL RK_gMpuLayoutFinalized = RK_FALSE;
static RK_MPU_DOMAIN_RESERVATION RK_gMpuDomainReservation[RK_NTHREADS];
static RK_MPU_SHARED_RESERVATION RK_gMpuSharedReservation[RK_NTHREADS];

/*
 * RK01 keeps power-of-two domain geometry even though ARMv8-M MPU regions are
 * limit based. That preserves the current API contract and avoids target-
 * dependent widening when the same application is built for v7-M and v8-M.
 */
static RK_BOOL kMpuIsPowerOfTwo_(ULONG const value)
{
    return (((value != 0UL) && ((value & (value - 1UL)) == 0UL)) ? RK_TRUE
                                                                 : RK_FALSE);
}

#define RK_MPU_MMFSR_IACCVIOL (1UL << 0U)
#define RK_MPU_MMFSR_DACCVIOL (1UL << 1U)
#define RK_MPU_MMFSR_MUNSTKERR (1UL << 3U)
#define RK_MPU_MMFSR_MSTKERR (1UL << 4U)
#define RK_MPU_MMFSR_MLSPERR (1UL << 5U)
#define RK_MPU_MMFSR_MMARVALID (1UL << 7U)
#define RK_MPU_MMFSR_FRAME_INVALID                                           \
    (RK_MPU_MMFSR_MUNSTKERR | RK_MPU_MMFSR_MSTKERR |                       \
     RK_MPU_MMFSR_MLSPERR)

/*
 * These MMFSR bits mean the stacked exception frame may itself be corrupt or
 * inaccessible. The MemManage path must not read stacked PC/LR when this mask
 * is set.
 */
static ULONG kMpuReadPsp_(VOID)
{
    ULONG value;
    RK_ASM volatile("MRS %0, PSP" : "=r"(value));
    return (value);
}

static ULONG kMpuReadControl_(VOID)
{
    ULONG value;
    RK_ASM volatile("MRS %0, CONTROL" : "=r"(value));
    return (value);
}

#if (RK_CONF_MPU_TASK_FAULT_PRINT_STDERR == ON)
static CHAR const *kMpuFaultAccessText_(ULONG const mmfsr)
{
    if ((mmfsr & RK_MPU_MMFSR_IACCVIOL) != 0UL)
    {
        return ("instruction access violation");
    }
    if ((mmfsr & RK_MPU_MMFSR_DACCVIOL) != 0UL)
    {
        return ("data access violation");
    }
    if ((mmfsr & RK_MPU_MMFSR_MSTKERR) != 0UL)
    {
        return ("stacking access violation");
    }
    if ((mmfsr & RK_MPU_MMFSR_MUNSTKERR) != 0UL)
    {
        return ("unstacking access violation");
    }
    if ((mmfsr & RK_MPU_MMFSR_MLSPERR) != 0UL)
    {
        return ("lazy fp access violation");
    }

    return ("memory access violation");
}

static VOID kMpuFaultPrint_(CHAR const *const prefixPtr)
{
    CHAR const *taskNamePtr = "?";

    if (RK_gRunPtr != NULL)
    {
        taskNamePtr = RK_gRunPtr->taskName;
    }

    if ((RK_gMpuFaultInfo.mmfsr & RK_MPU_MMFSR_MMARVALID) != 0UL)
    {
        printf("%s: %s task=%s tid=%u pc=0x%08lx lr=0x%08lx "
               "addr=0x%08lx cfsr=0x%08lx mmfsr=0x%02lx frame=%lu\r\n",
               prefixPtr,
               kMpuFaultAccessText_(RK_gMpuFaultInfo.mmfsr),
               taskNamePtr,
               (UINT)RK_gMpuFaultInfo.tid,
               RK_gMpuFaultInfo.pc,
               RK_gMpuFaultInfo.lr,
               RK_gMpuFaultInfo.faultAddress,
               RK_gMpuFaultInfo.cfsr,
               RK_gMpuFaultInfo.mmfsr,
               RK_gMpuFaultInfo.frameValid);
    }
    else
    {
        printf("%s: %s task=%s tid=%u pc=0x%08lx lr=0x%08lx "
               "addr=<none> cfsr=0x%08lx mmfsr=0x%02lx frame=%lu\r\n",
               prefixPtr,
               kMpuFaultAccessText_(RK_gMpuFaultInfo.mmfsr),
               taskNamePtr,
               (UINT)RK_gMpuFaultInfo.tid,
               RK_gMpuFaultInfo.pc,
               RK_gMpuFaultInfo.lr,
               RK_gMpuFaultInfo.cfsr,
               RK_gMpuFaultInfo.mmfsr,
               RK_gMpuFaultInfo.frameValid);
    }
}
#endif

RK_ERR kMpuBuildRegion(RK_MPU_REGION *const regionPtr,
                       UINT const regionNumber,
                       ULONG const baseAddress,
                       ULONG const regionSize,
                       ULONG const attributes)
{
    ULONG limitAddress;
    ULONG rbar;

    if (regionPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    regionPtr->rbar = 0UL;
    regionPtr->rlar = 0UL;

    if (regionNumber >= RK_MPU_N_REGIONS)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    if ((regionSize < 32UL) ||
        (kMpuIsPowerOfTwo_(regionSize) == RK_FALSE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    if ((baseAddress & (regionSize - 1UL)) != 0UL)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    if ((regionSize - 1UL) > (RK_ULONG_MAX - baseAddress))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    limitAddress = baseAddress + regionSize - 1UL;
    rbar = (baseAddress & RK_MPU_RBAR_BASE_MASK) |
           RK_MPU_RBAR_AP(RK_MPU_ATTR_DESC_AP(attributes)) |
           RK_MPU_RBAR_SH(RK_MPU_ATTR_DESC_SH(attributes));

    if (RK_MPU_ATTR_DESC_XN(attributes) != 0UL)
    {
        rbar |= RK_MPU_RBAR_XN;
    }

    /* Store precomputed RBAR/RLAR values in the TCB. Context switch can then
     * reload regions without recalculating alignment, size, or attributes. */
    regionPtr->rbar = rbar;
    regionPtr->rlar =
        (limitAddress & RK_MPU_RLAR_LIMIT_MASK) |
        RK_MPU_RLAR_ATTR_INDEX(RK_MPU_ATTR_DESC_INDEX(attributes)) |
        RK_MPU_RLAR_ENABLE;

    return (RK_ERR_SUCCESS);
}

RK_BOOL kMpuTaskMemoryValid(RK_TASK_MEMORY const *const memoryPtr)
{
    ULONG stackBytes;
    UINTPTR poolBegin;
    UINTPTR poolEnd;
    UINTPTR stackPoolBegin;
    UINTPTR stackPoolEnd;
    UINTPTR regionBegin;
    UINTPTR regionEnd;
    UINTPTR sharedBegin;
    UINTPTR sharedEnd;
    UINTPTR stackBegin;
    UINTPTR stackEnd;
    RK_BOOL const domainTask =
        ((memoryPtr != NULL) && (memoryPtr->domainPtr != NULL)) ?
        RK_TRUE : RK_FALSE;

    if (memoryPtr == NULL)
    {
        return (RK_FALSE);
    }

    if (memoryPtr->stackBasePtr == NULL)
    {
        return (RK_FALSE);
    }

    if ((memoryPtr->stackWords < RK_MIN_STACKSIZE) ||
        (memoryPtr->stackWords >
         (RK_ULONG_MAX / (ULONG)sizeof(RK_STACK))))
    {
        return (RK_FALSE);
    }

    stackBytes = memoryPtr->stackWords * (ULONG)sizeof(RK_STACK);

    if ((stackBytes < 32UL) ||
        (kMpuIsPowerOfTwo_(stackBytes) == RK_FALSE))
    {
        return (RK_FALSE);
    }

    poolBegin = (UINTPTR)&__rk_task_ram_begin;
    poolEnd = (UINTPTR)&__rk_task_ram_end;
    stackPoolBegin = (UINTPTR)&__rk_task_stack_begin;
    stackPoolEnd = (UINTPTR)&__rk_task_stack_end;
    stackBegin = (UINTPTR)memoryPtr->stackBasePtr;

    if ((stackBegin & ((UINTPTR)stackBytes - 1U)) != 0U)
    {
        return (RK_FALSE);
    }

    if ((stackBegin < stackPoolBegin) || (stackBegin >= stackPoolEnd))
    {
        return (RK_FALSE);
    }

    if ((UINTPTR)stackBytes > (stackPoolEnd - stackBegin))
    {
        return (RK_FALSE);
    }

    stackEnd = stackBegin + (UINTPTR)stackBytes;
    sharedBegin = (UINTPTR)&__rk_shared_ram_begin;
    sharedEnd = (UINTPTR)&__rk_shared_ram_end;

    if ((sharedEnd > sharedBegin) &&
        (stackBegin < sharedEnd) &&
        (stackEnd > sharedBegin))
    {
        return (RK_FALSE);
    }

    if (domainTask == RK_TRUE)
    {
        if ((memoryPtr->regionBasePtr == NULL) ||
            (memoryPtr->domainPtr->init != RK_TRUE) ||
            (memoryPtr->regionBasePtr != memoryPtr->domainPtr->regionBasePtr) ||
            (memoryPtr->regionBytes != memoryPtr->domainPtr->regionBytes) ||
            (kMpuDomainMemoryValid(memoryPtr->domainPtr) == RK_FALSE))
        {
            return (RK_FALSE);
        }

        regionBegin = (UINTPTR)memoryPtr->regionBasePtr;
        regionEnd = regionBegin + (UINTPTR)memoryPtr->regionBytes;

        if ((stackBegin < regionEnd) && (stackEnd > regionBegin))
        {
            return (RK_FALSE);
        }

        return (RK_TRUE);
    }

    if (memoryPtr->regionBasePtr == NULL)
    {
        return (RK_FALSE);
    }

    /* A private one-task arena is directly represented as an MPU region. */
    if ((memoryPtr->regionBytes < stackBytes) ||
        (memoryPtr->regionBytes < 32UL) ||
        (kMpuIsPowerOfTwo_(memoryPtr->regionBytes) == RK_FALSE))
    {
        return (RK_FALSE);
    }

    regionBegin = (UINTPTR)memoryPtr->regionBasePtr;

    if ((regionBegin & ((UINTPTR)memoryPtr->regionBytes - 1U)) != 0U)
    {
        return (RK_FALSE);
    }

    if ((regionBegin < poolBegin) || (regionBegin >= poolEnd))
    {
        return (RK_FALSE);
    }

    if ((UINTPTR)memoryPtr->regionBytes > (poolEnd - regionBegin))
    {
        return (RK_FALSE);
    }

    regionEnd = regionBegin + (UINTPTR)memoryPtr->regionBytes;

    if ((sharedEnd > sharedBegin) &&
        (regionBegin < sharedEnd) &&
        (regionEnd > sharedBegin))
    {
        return (RK_FALSE);
    }

    if ((stackBegin < regionBegin) || (stackBegin >= regionEnd))
    {
        return (RK_FALSE);
    }

    if ((UINTPTR)stackBytes > (regionEnd - stackBegin))
    {
        return (RK_FALSE);
    }

    return ((stackEnd == regionEnd) ? RK_TRUE : RK_FALSE);
}

RK_BOOL kMpuDomainMemoryValid(RK_DOMAIN const *const domainPtr)
{
    UINTPTR poolBegin;
    UINTPTR poolEnd;
    UINTPTR regionBegin;
    UINTPTR regionEnd;
    UINTPTR sharedBegin;
    UINTPTR sharedEnd;

    if ((domainPtr == NULL) || (domainPtr->regionBasePtr == NULL))
    {
        return (RK_FALSE);
    }

    if ((domainPtr->regionBytes < 32UL) ||
        (kMpuIsPowerOfTwo_(domainPtr->regionBytes) == RK_FALSE))
    {
        return (RK_FALSE);
    }

    poolBegin = (UINTPTR)&__rk_task_ram_begin;
    poolEnd = (UINTPTR)&__rk_task_ram_end;
    regionBegin = (UINTPTR)domainPtr->regionBasePtr;

    if ((regionBegin & ((UINTPTR)domainPtr->regionBytes - 1U)) != 0U)
    {
        return (RK_FALSE);
    }

    if ((regionBegin < poolBegin) || (regionBegin >= poolEnd))
    {
        return (RK_FALSE);
    }

    if ((UINTPTR)domainPtr->regionBytes > (poolEnd - regionBegin))
    {
        return (RK_FALSE);
    }

    regionEnd = regionBegin + (UINTPTR)domainPtr->regionBytes;
    sharedBegin = (UINTPTR)&__rk_shared_ram_begin;
    sharedEnd = (UINTPTR)&__rk_shared_ram_end;

    if ((sharedEnd > sharedBegin) &&
        (regionBegin < sharedEnd) &&
        (regionEnd > sharedBegin))
    {
        return (RK_FALSE);
    }

    return (RK_TRUE);
}

RK_BOOL kMpuSharedRegionMemoryValid(RK_SHARED_REGION const *const regionPtr)
{
    UINTPTR poolBegin;
    UINTPTR poolEnd;
    UINTPTR regionBegin;
    UINTPTR regionEnd;
    UINTPTR sharedBegin;
    UINTPTR sharedEnd;

    if ((regionPtr == NULL) || (regionPtr->regionBasePtr == NULL))
    {
        return (RK_FALSE);
    }

    if ((regionPtr->regionBytes < 32UL) ||
        (kMpuIsPowerOfTwo_(regionPtr->regionBytes) == RK_FALSE))
    {
        return (RK_FALSE);
    }

    poolBegin = (UINTPTR)&__rk_task_ram_begin;
    poolEnd = (UINTPTR)&__rk_task_ram_end;
    regionBegin = (UINTPTR)regionPtr->regionBasePtr;

    if ((regionBegin & ((UINTPTR)regionPtr->regionBytes - 1U)) != 0U)
    {
        return (RK_FALSE);
    }

    if ((regionBegin < poolBegin) || (regionBegin >= poolEnd))
    {
        return (RK_FALSE);
    }

    if ((UINTPTR)regionPtr->regionBytes > (poolEnd - regionBegin))
    {
        return (RK_FALSE);
    }

    regionEnd = regionBegin + (UINTPTR)regionPtr->regionBytes;
    sharedBegin = (UINTPTR)&__rk_shared_ram_begin;
    sharedEnd = (UINTPTR)&__rk_shared_ram_end;

    if ((sharedEnd > sharedBegin) &&
        (regionBegin < sharedEnd) &&
        (regionEnd > sharedBegin))
    {
        return (RK_FALSE);
    }

    return (RK_TRUE);
}

static RK_BOOL kMpuDomainReservationOverlaps_(RK_DOMAIN const *const domainPtr,
                                              BYTE const *const basePtr,
                                              ULONG const bytes)
{
    UINTPTR const begin = (UINTPTR)basePtr;
    UINTPTR const end = begin + (UINTPTR)bytes;

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_MPU_DOMAIN_RESERVATION const *const resPtr =
            &RK_gMpuDomainReservation[idx];

        if ((resPtr->domainPtr == NULL) || (resPtr->regionBasePtr == NULL) ||
            (resPtr->regionBytes == 0UL))
        {
            continue;
        }

        if (resPtr->domainPtr == domainPtr)
        {
            return (RK_TRUE);
        }

        UINTPTR const resBegin = (UINTPTR)resPtr->regionBasePtr;
        UINTPTR const resEnd = resBegin + (UINTPTR)resPtr->regionBytes;

        if ((begin < resEnd) && (end > resBegin))
        {
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static RK_BOOL
kMpuSharedReservationOverlaps_(RK_SHARED_REGION const *const regionPtr,
                               BYTE const *const basePtr,
                               ULONG const bytes)
{
    UINTPTR const begin = (UINTPTR)basePtr;
    UINTPTR const end = begin + (UINTPTR)bytes;

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_MPU_SHARED_RESERVATION const *const resPtr =
            &RK_gMpuSharedReservation[idx];

        if ((resPtr->regionPtr == NULL) || (resPtr->regionBasePtr == NULL) ||
            (resPtr->regionBytes == 0UL))
        {
            continue;
        }

        if (resPtr->regionPtr == regionPtr)
        {
            return (RK_TRUE);
        }

        UINTPTR const resBegin = (UINTPTR)resPtr->regionBasePtr;
        UINTPTR const resEnd = resBegin + (UINTPTR)resPtr->regionBytes;

        if ((begin < resEnd) && (end > resBegin))
        {
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static RK_BOOL kMpuSharedRegionReserved_(RK_SHARED_REGION const *const regionPtr)
{
    if (regionPtr == NULL)
    {
        return (RK_FALSE);
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_MPU_SHARED_RESERVATION const *const resPtr =
            &RK_gMpuSharedReservation[idx];

        if (resPtr->regionPtr == regionPtr)
        {
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static RK_BOOL kMpuRangeEndpoints_(VOID const *const ptr,
                                   ULONG const bytes,
                                   UINTPTR *const beginPtr,
                                   UINTPTR *const endPtr)
{
    /*
     * Convert pointer+length to a half-open interval [begin, end). The
     * overflow check is the important part: MPU validation must reject wrapped
     * ranges before doing containment tests.
     */
    UINTPTR begin;

    if ((beginPtr == NULL) || (endPtr == NULL))
    {
        return (RK_FALSE);
    }

    begin = (UINTPTR)ptr;
    *beginPtr = begin;
    *endPtr = begin;

    if (bytes == 0UL)
    {
        return (RK_TRUE);
    }

    if (ptr == NULL)
    {
        return (RK_FALSE);
    }

    if ((UINTPTR)bytes > ((UINTPTR)RK_ULONG_MAX - begin))
    {
        return (RK_FALSE);
    }

    *endPtr = begin + (UINTPTR)bytes;
    return (RK_TRUE);
}

static RK_BOOL kMpuRangeWithin_(UINTPTR const begin,
                                UINTPTR const end,
                                VOID const *const regionBasePtr,
                                ULONG const regionBytes)
{
    /*
     * Half-open range containment helper shared by domain RAM, global shared
     * RAM and explicit shared-region checks.
     */
    UINTPTR regionBegin;
    UINTPTR regionEnd;

    if ((regionBasePtr == NULL) || (regionBytes == 0UL))
    {
        return (RK_FALSE);
    }

    regionBegin = (UINTPTR)regionBasePtr;
    if ((UINTPTR)regionBytes > ((UINTPTR)RK_ULONG_MAX - regionBegin))
    {
        return (RK_FALSE);
    }

    regionEnd = regionBegin + (UINTPTR)regionBytes;
    return (((begin >= regionBegin) && (end <= regionEnd)) ? RK_TRUE
                                                           : RK_FALSE);
}

static RK_BOOL kMpuRangesOverlap_(BYTE const *const firstBasePtr,
                                  ULONG const firstBytes,
                                  BYTE const *const secondBasePtr,
                                  ULONG const secondBytes)
{
    /*
     * Layout validation is fail-closed: NULL, zero-sized and overflowing ranges
     * are reported as overlapping so invalid domain/shared declarations cannot
     * pass by looking empty.
     */
    UINTPTR const firstBegin = (UINTPTR)firstBasePtr;
    UINTPTR const secondBegin = (UINTPTR)secondBasePtr;
    UINTPTR firstEnd;
    UINTPTR secondEnd;

    if ((firstBasePtr == NULL) || (secondBasePtr == NULL) ||
        (firstBytes == 0UL) || (secondBytes == 0UL))
    {
        return (RK_TRUE);
    }

    if (((UINTPTR)firstBytes > ((UINTPTR)RK_ULONG_MAX - firstBegin)) ||
        ((UINTPTR)secondBytes > ((UINTPTR)RK_ULONG_MAX - secondBegin)))
    {
        return (RK_TRUE);
    }

    firstEnd = firstBegin + (UINTPTR)firstBytes;
    secondEnd = secondBegin + (UINTPTR)secondBytes;

    return (((firstBegin < secondEnd) && (firstEnd > secondBegin)) ?
            RK_TRUE : RK_FALSE);
}

static RK_BOOL kMpuTaskDataRangeValid_(RK_TCB const *const taskPtr,
                                       UINTPTR const begin,
                                       UINTPTR const end)
{
    UINTPTR const sharedBegin = (UINTPTR)&__rk_shared_ram_begin;
    UINTPTR const sharedEnd = (UINTPTR)&__rk_shared_ram_end;

    if ((taskPtr == NULL) || (taskPtr->init != RK_TRUE))
    {
        return (RK_FALSE);
    }

    if (kMpuRangeWithin_(begin, end, taskPtr->taskMemoryBasePtr,
                         taskPtr->taskMemoryBytes) == RK_TRUE)
    {
        return (RK_TRUE);
    }

    if (kMpuRangeWithin_(begin, end, taskPtr->stackBufPtr,
                         taskPtr->stackSize *
                         (ULONG)sizeof(RK_STACK)) == RK_TRUE)
    {
        return (RK_TRUE);
    }

    if ((sharedEnd > sharedBegin) &&
        (begin >= sharedBegin) &&
        (end <= sharedEnd))
    {
        return (RK_TRUE);
    }

    if (taskPtr->domainPtr == NULL)
    {
        return (RK_FALSE);
    }

    for (UINT idx = 0U; idx < RK_CONF_DOMAIN_SHARED_REGIONS; idx++)
    {
        RK_SHARED_REGION const *const regionPtr =
            taskPtr->domainPtr->sharedRegionPtr[idx];

        if ((regionPtr == NULL) || (regionPtr->init != RK_TRUE))
        {
            continue;
        }

        if (kMpuRangeWithin_(begin, end, regionPtr->regionBasePtr,
                             regionPtr->regionBytes) == RK_TRUE)
        {
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static RK_BOOL kMpuFlashRangeValid_(UINTPTR const begin, UINTPTR const end)
{
    UINTPTR const flashBegin = (UINTPTR)&__rk_flash_begin;
    UINTPTR const flashEnd = (UINTPTR)&__rk_flash_end;

    if (end < begin)
    {
        return (RK_FALSE);
    }

    return (((begin >= flashBegin) && (end <= flashEnd)) ? RK_TRUE
                                                         : RK_FALSE);
}

RK_BOOL kMpuUserReadValid(RK_TCB const *const taskPtr,
                          VOID const *const ptr,
                          ULONG const bytes)
{
    UINTPTR begin;
    UINTPTR end;

    if (kMpuRangeEndpoints_(ptr, bytes, &begin, &end) == RK_FALSE)
    {
        return (RK_FALSE);
    }

    if (bytes == 0UL)
    {
        return (RK_TRUE);
    }

    if (kMpuFlashRangeValid_(begin, end) == RK_TRUE)
    {
        return (RK_TRUE);
    }

    return (kMpuTaskDataRangeValid_(taskPtr, begin, end));
}

RK_BOOL kMpuUserWriteValid(RK_TCB const *const taskPtr,
                           VOID *const ptr,
                           ULONG const bytes)
{
    UINTPTR begin;
    UINTPTR end;

    if (kMpuRangeEndpoints_(ptr, bytes, &begin, &end) == RK_FALSE)
    {
        return (RK_FALSE);
    }

    if (bytes == 0UL)
    {
        return (RK_TRUE);
    }

    return (kMpuTaskDataRangeValid_(taskPtr, begin, end));
}

RK_BOOL kMpuUserFunctionValid(VOID const *const funPtr)
{
    UINTPTR const entry = (UINTPTR)funPtr;
    UINTPTR const addr = entry & ~((UINTPTR)1U);

    if ((funPtr == NULL) || ((entry & 1U) == 0U))
    {
        return (RK_FALSE);
    }

    return (kMpuFlashRangeValid_(addr, addr + 2U));
}

static RK_BOOL kMpuDomainReserved_(RK_DOMAIN const *const domainPtr)
{
    if (domainPtr == NULL)
    {
        return (RK_FALSE);
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_MPU_DOMAIN_RESERVATION const *const resPtr =
            &RK_gMpuDomainReservation[idx];

        if (resPtr->domainPtr == domainPtr)
        {
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static RK_BOOL kMpuTaskStackOverlapsLive_(RK_TCB const *const skipTaskPtr,
                                          RK_DOMAIN const *const domainPtr,
                                          RK_STACK const *const stackBasePtr,
                                          ULONG const stackWords)
{
    UINTPTR const stackBegin = (UINTPTR)stackBasePtr;
    UINTPTR const stackBytes = (UINTPTR)stackWords * sizeof(RK_STACK);
    UINTPTR const stackEnd = stackBegin + stackBytes;

    K_UNUSE(domainPtr);

    if ((stackBasePtr == NULL) ||
        (stackWords > (RK_ULONG_MAX / (ULONG)sizeof(RK_STACK))) ||
        (stackEnd < stackBegin))
    {
        return (RK_TRUE);
    }

    for (UINT idx = RK_N_SYSTASKS; idx < RK_NTHREADS; idx++)
    {
        RK_TCB const *const taskPtr = RK_gTaskHandleByPid[idx];

        if ((taskPtr == NULL) || (taskPtr == skipTaskPtr) ||
            (taskPtr->init != RK_TRUE) || (taskPtr->stackBufPtr == NULL) ||
            (taskPtr->stackSize == 0UL))
        {
            continue;
        }

        UINTPTR const otherBegin = (UINTPTR)taskPtr->stackBufPtr;
        UINTPTR const otherBytes =
            (UINTPTR)taskPtr->stackSize * sizeof(RK_STACK);
        UINTPTR const otherEnd = otherBegin + otherBytes;

        if ((otherEnd < otherBegin) ||
            ((stackBegin < otherEnd) && (stackEnd > otherBegin)))
        {
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static RK_BOOL kMpuDomainIsTaskPrivate_(RK_DOMAIN const *const domainPtr,
                                        RK_TCB const *const taskPtr)
{
    return (((domainPtr != NULL) && (taskPtr != NULL) &&
             (taskPtr->domainPtr == domainPtr) &&
             (domainPtr == &taskPtr->privateDomain)) ?
            RK_TRUE : RK_FALSE);
}

static RK_BOOL
kMpuDomainIsExactPrivateStack_(RK_DOMAIN const *const domainPtr,
                               BYTE const *const regionBasePtr,
                               ULONG const regionBytes)
{
    RK_TCB const *ownerPtr = NULL;
    ULONG stackBytes;

    if (domainPtr == NULL)
    {
        return (RK_FALSE);
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_TCB const *const taskPtr = RK_gTaskHandleByPid[idx];

        if ((taskPtr == NULL) || (taskPtr->init != RK_TRUE) ||
            (taskPtr->domainPtr != domainPtr))
        {
            continue;
        }

        if (ownerPtr != NULL)
        {
            return (RK_FALSE);
        }

        ownerPtr = taskPtr;
    }

    if ((ownerPtr == NULL) || (domainPtr != &ownerPtr->privateDomain) ||
        (domainPtr->taskCount != 1UL) || (ownerPtr->stackBufPtr == NULL) ||
        (ownerPtr->stackSize == 0UL) ||
        (ownerPtr->stackSize > (RK_ULONG_MAX / (ULONG)sizeof(RK_STACK))))
    {
        return (RK_FALSE);
    }

    stackBytes = ownerPtr->stackSize * (ULONG)sizeof(RK_STACK);

    return (((regionBasePtr == (BYTE const *)ownerPtr->stackBufPtr) &&
             (regionBytes == stackBytes) &&
             ((BYTE const *)domainPtr->regionBasePtr == regionBasePtr) &&
             (domainPtr->regionBytes == regionBytes)) ?
            RK_TRUE : RK_FALSE);
}

static RK_BOOL
kMpuAuthorityRegionOverlapsTaskStack_(RK_DOMAIN const *const domainPtr,
                                      BYTE const *const regionBasePtr,
                                      ULONG const regionBytes)
{
    RK_BOOL const exactPrivateStack =
        kMpuDomainIsExactPrivateStack_(domainPtr, regionBasePtr, regionBytes);
    UINTPTR const stackPoolBegin = (UINTPTR)&__rk_task_stack_begin;
    UINTPTR const stackPoolEnd = (UINTPTR)&__rk_task_stack_end;

    /* A domain/shared authority window must not expose any allocated or future
     * task stack. The isolated-task private domain is the one intentional
     * exact self-overlap. */
    if (stackPoolEnd < stackPoolBegin)
    {
        return (RK_TRUE);
    }

    if (stackPoolEnd > stackPoolBegin)
    {
        UINTPTR const stackPoolBytes = stackPoolEnd - stackPoolBegin;

        if ((stackPoolBytes > (UINTPTR)RK_ULONG_MAX) ||
            ((exactPrivateStack != RK_TRUE) &&
             (kMpuRangesOverlap_(regionBasePtr, regionBytes,
                                 (BYTE const *)stackPoolBegin,
                                 (ULONG)stackPoolBytes) == RK_TRUE)))
        {
            return (RK_TRUE);
        }
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_TCB const *const taskPtr = RK_gTaskHandleByPid[idx];

        if ((taskPtr == NULL) || (taskPtr->init != RK_TRUE) ||
            (taskPtr->stackBufPtr == NULL) || (taskPtr->stackSize == 0UL))
        {
            continue;
        }

        if (taskPtr->stackSize > (RK_ULONG_MAX / (ULONG)sizeof(RK_STACK)))
        {
            return (RK_TRUE);
        }

        if (kMpuRangesOverlap_(regionBasePtr, regionBytes,
                               (BYTE const *)taskPtr->stackBufPtr,
                               taskPtr->stackSize *
                                   (ULONG)sizeof(RK_STACK)) == RK_TRUE)
        {
            if ((exactPrivateStack == RK_TRUE) &&
                (kMpuDomainIsTaskPrivate_(domainPtr, taskPtr) == RK_TRUE))
            {
                continue;
            }

            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static ULONG kMpuLiveTaskCountForDomain_(RK_DOMAIN const *const domainPtr)
{
    ULONG count = 0UL;

    if (domainPtr == NULL)
    {
        return (0UL);
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_TCB const *const taskPtr = RK_gTaskHandleByPid[idx];

        if ((taskPtr != NULL) && (taskPtr->init == RK_TRUE) &&
            (taskPtr->domainPtr == domainPtr))
        {
            count++;
        }
    }

    return (count);
}

static RK_BOOL kMpuDomainSharedMappingsValid_(RK_DOMAIN const *const domainPtr)
{
    if ((domainPtr == NULL) || (domainPtr->init != RK_TRUE))
    {
        return (RK_FALSE);
    }

    for (UINT idx = 0U; idx < RK_CONF_DOMAIN_SHARED_REGIONS; idx++)
    {
        RK_SHARED_REGION const *const regionPtr =
            domainPtr->sharedRegionPtr[idx];

        if (regionPtr == NULL)
        {
            continue;
        }

        if ((regionPtr->init != RK_TRUE) ||
            (kMpuSharedRegionReserved_(regionPtr) != RK_TRUE) ||
            (kMpuSharedRegionMemoryValid(regionPtr) == RK_FALSE))
        {
            return (RK_FALSE);
        }

        for (UINT nextIdx = idx + 1U;
             nextIdx < RK_CONF_DOMAIN_SHARED_REGIONS;
             nextIdx++)
        {
            if (domainPtr->sharedRegionPtr[nextIdx] == regionPtr)
            {
                return (RK_FALSE);
            }
        }
    }

    return (RK_TRUE);
}

static RK_ERR kMpuLayoutValidateDomainReservations_(VOID)
{
    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_MPU_DOMAIN_RESERVATION const *const resPtr =
            &RK_gMpuDomainReservation[idx];

        if ((resPtr->domainPtr == NULL) &&
            (resPtr->regionBasePtr == NULL) &&
            (resPtr->regionBytes == 0UL))
        {
            continue;
        }

        if ((resPtr->domainPtr == NULL) ||
            (resPtr->regionBasePtr == NULL) ||
            (resPtr->regionBytes == 0UL) ||
            (resPtr->domainPtr->init != RK_TRUE) ||
            (resPtr->domainPtr->regionBasePtr != resPtr->regionBasePtr) ||
            (resPtr->domainPtr->regionBytes != resPtr->regionBytes))
        {
            return (RK_ERR_INVALID_OBJ);
        }

        if ((kMpuDomainMemoryValid(resPtr->domainPtr) == RK_FALSE) ||
            (kMpuDomainSharedMappingsValid_(resPtr->domainPtr) == RK_FALSE))
        {
            return (RK_ERR_INVALID_PARAM);
        }

        if (kMpuAuthorityRegionOverlapsTaskStack_(resPtr->domainPtr,
                                                  resPtr->regionBasePtr,
                                                  resPtr->regionBytes) ==
            RK_TRUE)
        {
            return (RK_ERR_INVALID_OBJ);
        }

        for (UINT nextIdx = idx + 1U; nextIdx < RK_NTHREADS; nextIdx++)
        {
            RK_MPU_DOMAIN_RESERVATION const *const nextPtr =
                &RK_gMpuDomainReservation[nextIdx];

            if ((nextPtr->domainPtr == NULL) &&
                (nextPtr->regionBasePtr == NULL) &&
                (nextPtr->regionBytes == 0UL))
            {
                continue;
            }

            if ((nextPtr->domainPtr == resPtr->domainPtr) ||
                (kMpuRangesOverlap_(resPtr->regionBasePtr,
                                    resPtr->regionBytes,
                                    nextPtr->regionBasePtr,
                                    nextPtr->regionBytes) == RK_TRUE))
            {
                return (RK_ERR_INVALID_OBJ);
            }
        }

        for (UINT sharedIdx = 0U; sharedIdx < RK_NTHREADS; sharedIdx++)
        {
            RK_MPU_SHARED_RESERVATION const *const sharedPtr =
                &RK_gMpuSharedReservation[sharedIdx];

            if ((sharedPtr->regionPtr == NULL) &&
                (sharedPtr->regionBasePtr == NULL) &&
                (sharedPtr->regionBytes == 0UL))
            {
                continue;
            }

            if (kMpuRangesOverlap_(resPtr->regionBasePtr,
                                   resPtr->regionBytes,
                                   sharedPtr->regionBasePtr,
                                   sharedPtr->regionBytes) == RK_TRUE)
            {
                return (RK_ERR_INVALID_OBJ);
            }
        }

        if (resPtr->domainPtr->taskCount !=
            kMpuLiveTaskCountForDomain_(resPtr->domainPtr))
        {
            return (RK_ERR_INVALID_OBJ);
        }
    }

    return (RK_ERR_SUCCESS);
}

static RK_ERR kMpuLayoutValidateSharedReservations_(VOID)
{
    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_MPU_SHARED_RESERVATION const *const resPtr =
            &RK_gMpuSharedReservation[idx];

        if ((resPtr->regionPtr == NULL) &&
            (resPtr->regionBasePtr == NULL) &&
            (resPtr->regionBytes == 0UL))
        {
            continue;
        }

        if ((resPtr->regionPtr == NULL) ||
            (resPtr->regionBasePtr == NULL) ||
            (resPtr->regionBytes == 0UL) ||
            (resPtr->regionPtr->init != RK_TRUE) ||
            (resPtr->regionPtr->regionBasePtr != resPtr->regionBasePtr) ||
            (resPtr->regionPtr->regionBytes != resPtr->regionBytes))
        {
            return (RK_ERR_INVALID_OBJ);
        }

        if (kMpuSharedRegionMemoryValid(resPtr->regionPtr) == RK_FALSE)
        {
            return (RK_ERR_INVALID_PARAM);
        }

        if (kMpuAuthorityRegionOverlapsTaskStack_(NULL,
                                                  resPtr->regionBasePtr,
                                                  resPtr->regionBytes) ==
            RK_TRUE)
        {
            return (RK_ERR_INVALID_OBJ);
        }

        for (UINT nextIdx = idx + 1U; nextIdx < RK_NTHREADS; nextIdx++)
        {
            RK_MPU_SHARED_RESERVATION const *const nextPtr =
                &RK_gMpuSharedReservation[nextIdx];

            if ((nextPtr->regionPtr == NULL) &&
                (nextPtr->regionBasePtr == NULL) &&
                (nextPtr->regionBytes == 0UL))
            {
                continue;
            }

            if ((nextPtr->regionPtr == resPtr->regionPtr) ||
                (kMpuRangesOverlap_(resPtr->regionBasePtr,
                                    resPtr->regionBytes,
                                    nextPtr->regionBasePtr,
                                    nextPtr->regionBytes) == RK_TRUE))
            {
                return (RK_ERR_INVALID_OBJ);
            }
        }
    }

    return (RK_ERR_SUCCESS);
}

static RK_ERR kMpuLayoutValidateTask_(RK_TCB const *const taskPtr)
{
    RK_TASK_MEMORY memory;
    RK_BOOL const privateDomainTask =
        ((taskPtr != NULL) && (taskPtr->domainPtr == &taskPtr->privateDomain)) ?
        RK_TRUE : RK_FALSE;

    if ((taskPtr == NULL) || (taskPtr->init != RK_TRUE))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if ((taskPtr->tid >= RK_NTHREADS) ||
        (RK_gTaskHandleByPid[taskPtr->tid] != taskPtr))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if ((taskPtr->stackBufPtr == NULL) ||
        (taskPtr->stackSize < RK_MIN_STACKSIZE) ||
        ((taskPtr->stackSize & 1UL) != 0UL) ||
        (taskPtr->stackSize > (RK_ULONG_MAX / (ULONG)sizeof(RK_STACK))) ||
        (((UINTPTR)taskPtr->stackBufPtr & 0x7U) != 0U))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    if (((taskPtr->savedControl & 0x1UL) == 0UL) &&
        (taskPtr->domainPtr == NULL) &&
        (taskPtr->taskMemoryBasePtr == NULL) &&
        (taskPtr->taskMemoryBytes == 0UL))
    {
        return (RK_ERR_SUCCESS);
    }

    if ((taskPtr->domainPtr == NULL) ||
        (taskPtr->taskMemoryBasePtr == NULL) ||
        (taskPtr->taskMemoryBytes == 0UL))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    memory.regionBasePtr = taskPtr->taskMemoryBasePtr;
    memory.regionBytes = taskPtr->taskMemoryBytes;
    memory.stackBasePtr = taskPtr->stackBufPtr;
    memory.stackWords = taskPtr->stackSize;
    memory.domainPtr = (privateDomainTask == RK_TRUE) ? NULL :
                       taskPtr->domainPtr;

    if ((kMpuDomainReserved_(taskPtr->domainPtr) != RK_TRUE) ||
        (kMpuTaskMemoryValid(&memory) == RK_FALSE))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if ((privateDomainTask != RK_TRUE) &&
        (kMpuDomainSharedMappingsValid_(taskPtr->domainPtr) == RK_FALSE))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if (kMpuTaskStackOverlapsLive_(taskPtr, taskPtr->domainPtr,
                                   taskPtr->stackBufPtr,
                                   taskPtr->stackSize) == RK_TRUE)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    return (RK_ERR_SUCCESS);
}

static RK_ERR kMpuLayoutValidateTasks_(VOID)
{
    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_TCB const *const taskPtr = RK_gTaskHandleByPid[idx];

        if (taskPtr == NULL)
        {
            continue;
        }

        RK_ERR const err = kMpuLayoutValidateTask_(taskPtr);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
    }

    return (RK_ERR_SUCCESS);
}

RK_BOOL kMpuLayoutIsFinalized(VOID)
{
    return ((RK_gMpuLayoutFinalized == RK_TRUE) ? RK_TRUE : RK_FALSE);
}

RK_ERR kMpuLayoutFinalize(VOID)
{
    RK_ERR err;

    if (RK_gMpuLayoutFinalized == RK_TRUE)
    {
        return (RK_ERR_SUCCESS);
    }

    err = kMpuLayoutValidateDomainReservations_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kMpuLayoutValidateSharedReservations_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kMpuLayoutValidateTasks_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    RK_DSB
    RK_gMpuLayoutFinalized = RK_TRUE;
    RK_DSB
    return (RK_ERR_SUCCESS);
}

static VOID kMpuDomainMemoryRelease_(RK_DOMAIN *const domainPtr)
{
    if (domainPtr == NULL)
    {
        return;
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_MPU_DOMAIN_RESERVATION *const resPtr =
            &RK_gMpuDomainReservation[idx];

        if (resPtr->domainPtr == domainPtr)
        {
            resPtr->domainPtr = NULL;
            resPtr->regionBasePtr = NULL;
            resPtr->regionBytes = 0UL;
        }
    }
}

RK_ERR kMpuDomainMemoryReserve(RK_DOMAIN *const domainPtr)
{
    if (domainPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (RK_gMpuLayoutFinalized == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    if (kMpuDomainMemoryValid(domainPtr) == RK_FALSE)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    if ((kMpuDomainReservationOverlaps_(domainPtr, domainPtr->regionBasePtr,
                                        domainPtr->regionBytes) == RK_TRUE) ||
        (kMpuSharedReservationOverlaps_(NULL, domainPtr->regionBasePtr,
                                        domainPtr->regionBytes) == RK_TRUE))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_MPU_DOMAIN_RESERVATION *const resPtr =
            &RK_gMpuDomainReservation[idx];

        if (resPtr->domainPtr == NULL)
        {
            resPtr->domainPtr = domainPtr;
            resPtr->regionBasePtr = domainPtr->regionBasePtr;
            resPtr->regionBytes = domainPtr->regionBytes;
            return (RK_ERR_SUCCESS);
        }
    }

    return (RK_ERR_TASK_POOL_EMPTY);
}

RK_ERR kMpuSharedRegionMemoryReserve(RK_SHARED_REGION *const regionPtr)
{
    if (regionPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (RK_gMpuLayoutFinalized == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    if (kMpuSharedRegionMemoryValid(regionPtr) == RK_FALSE)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    if ((kMpuDomainReservationOverlaps_(NULL, regionPtr->regionBasePtr,
                                        regionPtr->regionBytes) == RK_TRUE) ||
        (kMpuSharedReservationOverlaps_(regionPtr, regionPtr->regionBasePtr,
                                        regionPtr->regionBytes) == RK_TRUE))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_MPU_SHARED_RESERVATION *const resPtr =
            &RK_gMpuSharedReservation[idx];

        if (resPtr->regionPtr == NULL)
        {
            resPtr->regionPtr = regionPtr;
            resPtr->regionBasePtr = regionPtr->regionBasePtr;
            resPtr->regionBytes = regionPtr->regionBytes;
            return (RK_ERR_SUCCESS);
        }
    }

    return (RK_ERR_TASK_POOL_EMPTY);
}

VOID kMpuSharedRegionMemoryRelease(RK_SHARED_REGION *const regionPtr)
{
    if (regionPtr == NULL)
    {
        return;
    }

    for (UINT idx = 0U; idx < RK_NTHREADS; idx++)
    {
        RK_MPU_SHARED_RESERVATION *const resPtr =
            &RK_gMpuSharedReservation[idx];

        if (resPtr->regionPtr == regionPtr)
        {
            resPtr->regionPtr = NULL;
            resPtr->regionBasePtr = NULL;
            resPtr->regionBytes = 0UL;
        }
    }
}

RK_ERR kMpuTaskMemoryReserve(RK_TCB *const taskPtr,
                             RK_TASK_MEMORY const *const memoryPtr)
{
    RK_TASK_MEMORY taskMemory;
    RK_DOMAIN *domainPtr;

    if ((taskPtr == NULL) || (memoryPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (kMpuTaskMemoryValid(memoryPtr) == RK_FALSE)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    if ((taskPtr->taskMemoryBasePtr != NULL) || (taskPtr->domainPtr != NULL))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    taskMemory = *memoryPtr;
    domainPtr = memoryPtr->domainPtr;

    if (domainPtr == NULL)
    {
        if (RK_gMpuLayoutFinalized == RK_TRUE)
        {
            return (RK_ERR_INVALID_PHASE);
        }

        taskPtr->privateDomain.regionBasePtr = memoryPtr->regionBasePtr;
        taskPtr->privateDomain.regionBytes = memoryPtr->regionBytes;
        taskPtr->privateDomain.init = RK_FALSE;

        RK_ERR const err = kMpuDomainMemoryReserve(&taskPtr->privateDomain);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }

        taskPtr->privateDomain.init = RK_TRUE;
        domainPtr = &taskPtr->privateDomain;
    }

    if (kMpuDomainReserved_(domainPtr) != RK_TRUE)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if (kMpuTaskMemoryValid(&taskMemory) == RK_FALSE)
    {
        if (domainPtr == &taskPtr->privateDomain)
        {
            kMpuDomainMemoryRelease_(&taskPtr->privateDomain);
            RK_MEMSET(&taskPtr->privateDomain, 0, sizeof(RK_DOMAIN));
        }
        return (RK_ERR_INVALID_PARAM);
    }

    if (kMpuTaskStackOverlapsLive_(taskPtr, domainPtr,
                                   taskMemory.stackBasePtr,
                                   taskMemory.stackWords) == RK_TRUE)
    {
        if (domainPtr == &taskPtr->privateDomain)
        {
            kMpuDomainMemoryRelease_(&taskPtr->privateDomain);
            RK_MEMSET(&taskPtr->privateDomain, 0, sizeof(RK_DOMAIN));
        }
        return (RK_ERR_INVALID_OBJ);
    }

    taskPtr->taskMemoryBasePtr = taskMemory.regionBasePtr;
    taskPtr->taskMemoryBytes = taskMemory.regionBytes;
    taskPtr->domainPtr = domainPtr;
    domainPtr->taskCount++;
    return (RK_ERR_SUCCESS);
}

RK_ERR kMpuTaskAttachSharedRegions(RK_TCB *const taskPtr)
{
    ULONG const sharedBegin = (ULONG)(UINTPTR)&__rk_shared_ram_begin;
    ULONG const sharedBytes =
        (ULONG)((UINTPTR)&__rk_shared_ram_end -
                (UINTPTR)&__rk_shared_ram_begin);
    RK_ERR err = RK_ERR_SUCCESS;

    if (taskPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (sharedBytes != 0UL)
    {
        err = kMpuBuildRegion(&taskPtr->mpuRegion[RK_MPU_REGION_SHARED_RAM],
                              RK_MPU_REGION_SHARED_RAM,
                              sharedBegin,
                              sharedBytes,
                              RK_MPU_ATTR_USER_SRAM);

        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
    }

    if (taskPtr->domainPtr == NULL)
    {
        return (RK_ERR_SUCCESS);
    }

    for (UINT idx = 0U; idx < RK_CONF_DOMAIN_SHARED_REGIONS; idx++)
    {
        RK_SHARED_REGION *const regionPtr =
            taskPtr->domainPtr->sharedRegionPtr[idx];

        if (regionPtr == NULL)
        {
            continue;
        }

        if ((regionPtr->init != RK_TRUE) ||
            (kMpuSharedRegionReserved_(regionPtr) != RK_TRUE))
        {
            return (RK_ERR_INVALID_OBJ);
        }

        err = kMpuBuildRegion(
            &taskPtr->mpuRegion[RK_MPU_REGION_EXPLICIT_SHARED_BASE + idx],
            RK_MPU_REGION_EXPLICIT_SHARED_BASE + idx,
            (ULONG)(UINTPTR)regionPtr->regionBasePtr,
            regionPtr->regionBytes,
            RK_MPU_ATTR_USER_SRAM);

        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
    }

    return (RK_ERR_SUCCESS);
}

VOID kMpuTaskMemoryRelease(RK_TCB *const taskPtr)
{
    RK_DOMAIN *const domainPtr = (taskPtr != NULL) ? taskPtr->domainPtr : NULL;

    if (taskPtr == NULL)
    {
        return;
    }

    if ((domainPtr != NULL) && (domainPtr->taskCount > 0UL))
    {
        domainPtr->taskCount--;
    }

    /* Called during task teardown/reuse so a later isolated task can reserve
     * the same private one-task domain RAM block. Explicit domains outlive
     * their member tasks. */
    if (domainPtr == &taskPtr->privateDomain)
    {
        kMpuDomainMemoryRelease_(&taskPtr->privateDomain);
        RK_MEMSET(&taskPtr->privateDomain, 0, sizeof(RK_DOMAIN));
    }

    taskPtr->taskMemoryBasePtr = NULL;
    taskPtr->taskMemoryBytes = 0UL;
    taskPtr->domainPtr = NULL;
}

VOID kMpuInit(VOID)
{
    RK_MPU_REGION flashRegion;
    UINT const regionCount = (UINT)RK_MPU_TYPE_DREGION(RK_REG_MPU_TYPE);
    ULONG const flashBegin = (ULONG)(UINTPTR)&__rk_flash_begin;
    ULONG const flashBytes =
        (ULONG)((UINTPTR)&__rk_flash_end - (UINTPTR)&__rk_flash_begin);

    /* RK01 assumes at least eight programmable M-profile MPU regions. */
    if (regionCount < RK_MPU_N_REGIONS)
    {
        K_ERR_HANDLER(RK_GENERIC_FAULT);
        return;
    }

    if (kMpuBuildRegion(&flashRegion, RK_MPU_REGION_USER_FLASH, flashBegin,
                        flashBytes, RK_MPU_ATTR_USER_FLASH) != RK_ERR_SUCCESS)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        return;
    }

    /* Program from a known disabled state, then install the permanent user
     * FLASH region before enabling faults and the MPU. */
    RK_DSB
    RK_REG_MPU_CTRL = 0UL;
    RK_DSB
    RK_ISB

    RK_REG_MPU_MAIR0 =
        (RK_MPU_MAIR_NORMAL_NC << (RK_MPU_ATTR_INDEX_NORMAL * 8U)) |
        (RK_MPU_MAIR_DEVICE_NGNRE << (RK_MPU_ATTR_INDEX_DEVICE * 8U));
    RK_REG_MPU_MAIR1 = 0UL;

    for (UINT regionNumber = 0U; regionNumber < RK_MPU_N_REGIONS;
         regionNumber++)
    {
        RK_REG_MPU_RNR = regionNumber;
        RK_REG_MPU_RLAR = 0UL;
    }

    RK_REG_MPU_RNR = RK_MPU_REGION_USER_FLASH;
    RK_REG_MPU_RBAR = flashRegion.rbar;
    RK_REG_MPU_RLAR = flashRegion.rlar;

    /* PRIVDEFENA keeps privileged kernel code using the normal system map while
     * unprivileged tasks are constrained to configured MPU regions. */
    RK_REG_SCB_SHCSR |= RK_SCB_SHCSR_MEMFAULTENA;
    RK_REG_MPU_CTRL = RK_MPU_CTRL_PRIVDEFENA | RK_MPU_CTRL_ENABLE;
    RK_gMpuActiveTaskPtr = NULL;
    RK_gMpuActiveDomainMapPtr = NULL;

    RK_DSB
    RK_ISB
}

VOID kMpuLoadRunTask(VOID)
{
    RK_TCB *const taskPtr = RK_gRunPtr;
    RK_BOOL const sameDomainMap =
        ((taskPtr != NULL) &&
         (taskPtr->domainPtr == RK_gMpuActiveDomainMapPtr)) ?
        RK_TRUE : RK_FALSE;

    if (taskPtr == NULL)
    {
        return;
    }

    /* PendSV/startup may ask to load the same task repeatedly. Avoid rewriting
     * MPU registers when the active task map is already correct. */
    if (taskPtr == RK_gMpuActiveTaskPtr)
    {
        return;
    }

    RK_DSB

    if (sameDomainMap != RK_TRUE)
    {
        /* Region 0 is static FLASH. Region 2 is task-private stack. The
         * remaining slots form the immutable domain authority map. */
        for (UINT regionNumber = 1U; regionNumber < RK_MPU_N_REGIONS;
             regionNumber++)
        {
            RK_MPU_REGION const *regionPtr;

            if (regionNumber == RK_MPU_REGION_TASK_STACK)
            {
                continue;
            }

            regionPtr = &taskPtr->mpuRegion[regionNumber];

            RK_REG_MPU_RNR = regionNumber;
            RK_REG_MPU_RLAR = 0UL;

            if (regionPtr->rlar != 0UL)
            {
                RK_REG_MPU_RBAR = regionPtr->rbar;
                RK_REG_MPU_RLAR = regionPtr->rlar;
            }
        }

        RK_gMpuActiveDomainMapPtr = taskPtr->domainPtr;
    }

    RK_MPU_REGION const *const stackRegionPtr =
        &taskPtr->mpuRegion[RK_MPU_REGION_TASK_STACK];

    RK_REG_MPU_RNR = RK_MPU_REGION_TASK_STACK;
    RK_REG_MPU_RLAR = 0UL;

    if (stackRegionPtr->rlar != 0UL)
    {
        RK_REG_MPU_RBAR = stackRegionPtr->rbar;
        RK_REG_MPU_RLAR = stackRegionPtr->rlar;
    }

    RK_gMpuActiveTaskPtr = taskPtr;

    RK_DSB
    RK_ISB
}

VOID kMpuSaveRunTaskControl(ULONG const control)
{
    /* PendSV saves CONTROL so the next restore returns the task with the same
     * PSP/privilege state it had before switching out. */
    if (RK_gRunPtr != NULL)
    {
        RK_gRunPtr->savedControl = control;
    }
}

ULONG kMpuGetRunTaskControl(VOID)
{
    /* Before the first task is running, default to privileged PSP. */
    if (RK_gRunPtr == NULL)
    {
        return (RK_CONTROL_PSP_PRIVILEGED);
    }

    return (RK_gRunPtr->savedControl);
}

VOID kMemManageFault(RK_EXCEPTION_FRAME *const framePtr,
                     ULONG const excReturn)
{
    ULONG faultAddress = 0UL;
    ULONG const cfsr = RK_REG_SCB_CFSR;
    ULONG const mmfsr = cfsr & 0xFFUL;
    ULONG const psp = kMpuReadPsp_();
    ULONG const control = kMpuReadControl_();
    RK_BOOL const frameValid =
        (((mmfsr & RK_MPU_MMFSR_FRAME_INVALID) == 0UL) &&
         (framePtr != NULL)) ? RK_TRUE : RK_FALSE;
    RK_BOOL const faultFatal =
        (((excReturn & 0x4UL) == 0UL) || (RK_gRunPtr == NULL) ||
         (((control & 0x1UL) == 0UL) &&
          ((RK_gRunPtr->savedControl & 0x1UL) == 0UL))) ? RK_TRUE : RK_FALSE;

    /* MMARVALID indicates that MMFAR contains the faulting data address. Some
     * execute or stacking faults may not provide a useful address. */
    if ((mmfsr & RK_MPU_MMFSR_MMARVALID) != 0UL)
    {
        faultAddress = RK_REG_SCB_MMFAR;
    }

    RK_gMpuFaultInfo.tid =
        (RK_gRunPtr != NULL) ? RK_gRunPtr->tid : (RK_TID)UINT8_MAX;
    RK_gMpuFaultInfo.pc = (frameValid == RK_TRUE) ? framePtr->pc : 0UL;
    RK_gMpuFaultInfo.lr = (frameValid == RK_TRUE) ? framePtr->lr : 0UL;
    RK_gMpuFaultInfo.xpsr = (frameValid == RK_TRUE) ? framePtr->xpsr : 0UL;
    RK_gMpuFaultInfo.cfsr = cfsr;
    RK_gMpuFaultInfo.mmfsr = mmfsr;
    RK_gMpuFaultInfo.faultAddress = faultAddress;
    RK_gMpuFaultInfo.excReturn = excReturn;
    RK_gMpuFaultInfo.psp = psp;
    RK_gMpuFaultInfo.control = control;
    RK_gMpuFaultInfo.frameValid = frameValid;

    RK_REG_SCB_CFSR = mmfsr;
    RK_DSB
    RK_ISB

#if (RK_CONF_MPU_TASK_FAULT_PRINT_STDERR == ON)
    kMpuFaultPrint_((faultFatal == RK_TRUE) ? "MPU FATAL" : "MPU TASK FAULT");
#endif

    /* A fault in privileged handler/kernel context is fatal. A fault from an
     * unprivileged task is contained by terminating that task and rescheduling. */
    if (faultFatal == RK_TRUE)
    {
        K_ERR_HANDLER(RK_FAULT_MEM_ACCESS);
        while (1)
        {
        }
    }

#if (RK_CONF_MPU_TASK_FAULT_FAIL_FAST == ON)
    K_PANIC("MPU TASK FAULT tid=%u pc=0x%08lx lr=0x%08lx xpsr=0x%08lx "
            "cfsr=0x%08lx mmfsr=0x%02lx addr=0x%08lx psp=0x%08lx "
            "control=0x%08lx frame=%lu",
            (UINT)RK_gMpuFaultInfo.tid,
            RK_gMpuFaultInfo.pc,
            RK_gMpuFaultInfo.lr,
            RK_gMpuFaultInfo.xpsr,
            RK_gMpuFaultInfo.cfsr,
            RK_gMpuFaultInfo.mmfsr,
            RK_gMpuFaultInfo.faultAddress,
            RK_gMpuFaultInfo.psp,
            RK_gMpuFaultInfo.control,
            RK_gMpuFaultInfo.frameValid);
    RK_ASM volatile("CPSID I" : : : "memory");
    RK_ASM volatile("BKPT #0");
    while (1)
    {
        RK_ASM volatile("NOP");
    }
#endif

    if (kTaskFaultTerminate(RK_gRunPtr) != RK_ERR_SUCCESS)
    {
        K_ERR_HANDLER(RK_GENERIC_FAULT);
        while (1)
        {
        }
    }
}


RK_FUNC_WEAK
VOID kSyscallDispatch(RK_EXCEPTION_FRAME *const framePtr,
                      ULONG const excReturn,
                      ULONG const svcNumber)
{
    K_UNUSE(excReturn);
    K_UNUSE(svcNumber);

    if (framePtr != NULL)
    {
        framePtr->r0 = (ULONG)RK_ERR_INVALID_PARAM;
    }
}
