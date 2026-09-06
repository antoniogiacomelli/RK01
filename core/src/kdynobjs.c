/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * File intent:
 *   Fixed-pool runtime object registry. It allocates per-family kernel object
 *   slots, publishes encoded type/slot/generation handles and rejects stale
 *   handles before object memory is reused.
 *
 * Contracts/invariants:
 *   - Public Create APIs receive a handle cell that must be RK_NULL_HANDLE
 *     on entry.
 *   - A published RK_HANDLE is a tagged token, never a dereferenceable pointer.
 *   - A live dynamic object must have matching type, slot, generation,
 *     registry entry, pool membership and object header.
 *   - Destroy invalidates the registry and bumps generation before the pool
 *     slot can be reused.
 */

#define RK_SOURCE_CODE

#include <kdynobjs.h>
#include <kerr.h>
#include <kmem.h>
#include <kmesgq.h>
#include <kmrm.h>
#include <kmutex.h>
#include <ksema.h>
#include <ksch.h>
#include <ksharedmem.h>
#include <ksleepq.h>
#include <kstring.h>
#include <ksyscall.h>
#include <ktimer.h>
#include <ktrace.h>

static RK_BOOL dynObjPartitionsInit;

typedef struct RK_STRUCT_DYN_OBJ_CLASS
{
    RK_MEM_PARTITION *partPtr;
    VOID **registryPtr;
    USHORT *generationPtr;
    ULONG maxObjects;
    ULONG objSize;
    RK_ID objID;
} RK_DYN_OBJ_CLASS_;

#define RK_DYN_OBJ_HAS_ANY_POOL                                              \
    (((RK_CONF_SEMAPHORE == ON) && (RK_CONF_DYNAMIC_SEMAPHORES_MAX > 0U)) || \
     ((RK_CONF_MUTEX == ON) && (RK_CONF_DYNAMIC_MUTEXES_MAX > 0U)) ||         \
     ((RK_CONF_SLEEP_QUEUE == ON) &&                                         \
      (RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX > 0U)) ||                            \
     ((RK_CONF_MESG_QUEUE == ON) &&                                          \
      (RK_CONF_DYNAMIC_MESG_QUEUES_MAX > 0U)) ||                             \
     ((RK_CONF_CALLOUT_TIMER == ON) && (RK_CONF_DYNAMIC_TIMERS_MAX > 0U)) ||  \
     ((RK_CONF_MRM == ON) && (RK_CONF_DYNAMIC_MRMS_MAX > 0U)) ||              \
     (RK_CONF_DYNAMIC_SHARED_MEMS_MAX > 0U))

#define RK_DYN_OBJ_HAS_STATEFUL_DESTROY                                      \
    (((RK_CONF_SEMAPHORE == ON) && (RK_CONF_DYNAMIC_SEMAPHORES_MAX > 0U)) || \
     ((RK_CONF_MUTEX == ON) && (RK_CONF_DYNAMIC_MUTEXES_MAX > 0U)) ||         \
     ((RK_CONF_SLEEP_QUEUE == ON) &&                                         \
      (RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX > 0U)) ||                            \
     ((RK_CONF_MESG_QUEUE == ON) &&                                          \
      (RK_CONF_DYNAMIC_MESG_QUEUES_MAX > 0U)) ||                             \
     ((RK_CONF_MRM == ON) && (RK_CONF_DYNAMIC_MRMS_MAX > 0U)) ||              \
     (RK_CONF_DYNAMIC_SHARED_MEMS_MAX > 0U))

/*
 * Each runtime object family has its own fixed partition, registry and
 * generation table. RK_HANDLE_SLOT_MASK is eight bits wide, so each family is
 * capped at 256 live slots.
 */
#if ((RK_CONF_SEMAPHORE == ON) && (RK_CONF_DYNAMIC_SEMAPHORES_MAX > 0U))
#if (RK_CONF_DYNAMIC_SEMAPHORES_MAX > 256U)
#error "RK_CONF_DYNAMIC_SEMAPHORES_MAX exceeds opaque handle slot capacity"
#endif
static RK_MEM_PARTITION dynSemaPart;
static ULONG dynSemaPool[RK_CONF_DYNAMIC_SEMAPHORES_MAX]
                         [RK_TYPE_WORD_COUNT(RK_SEMAPHORE)] K_ALIGN(4);
static VOID *dynSemaRegistry[RK_CONF_DYNAMIC_SEMAPHORES_MAX];
static USHORT dynSemaGeneration[RK_CONF_DYNAMIC_SEMAPHORES_MAX];
#endif

#if ((RK_CONF_MUTEX == ON) && (RK_CONF_DYNAMIC_MUTEXES_MAX > 0U))
#if (RK_CONF_DYNAMIC_MUTEXES_MAX > 256U)
#error "RK_CONF_DYNAMIC_MUTEXES_MAX exceeds opaque handle slot capacity"
#endif
static RK_MEM_PARTITION dynMutexPart;
static ULONG dynMutexPool[RK_CONF_DYNAMIC_MUTEXES_MAX]
                          [RK_TYPE_WORD_COUNT(RK_MUTEX)] K_ALIGN(4);
static VOID *dynMutexRegistry[RK_CONF_DYNAMIC_MUTEXES_MAX];
static USHORT dynMutexGeneration[RK_CONF_DYNAMIC_MUTEXES_MAX];
#endif

#if ((RK_CONF_SLEEP_QUEUE == ON) && (RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX > 0U))
#if (RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX > 256U)
#error "RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX exceeds opaque handle slot capacity"
#endif
static RK_MEM_PARTITION dynSleepqPart;
static ULONG dynSleepqPool[RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX]
                           [RK_TYPE_WORD_COUNT(RK_SLEEP_QUEUE)] K_ALIGN(4);
static VOID *dynSleepqRegistry[RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX];
static USHORT dynSleepqGeneration[RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX];
#endif

#if ((RK_CONF_MESG_QUEUE == ON) && (RK_CONF_DYNAMIC_MESG_QUEUES_MAX > 0U))
#if (RK_CONF_DYNAMIC_MESG_QUEUES_MAX > 256U)
#error "RK_CONF_DYNAMIC_MESG_QUEUES_MAX exceeds opaque handle slot capacity"
#endif
static RK_MEM_PARTITION dynMesgqPart;
static ULONG dynMesgqPool[RK_CONF_DYNAMIC_MESG_QUEUES_MAX]
                          [RK_TYPE_WORD_COUNT(RK_MESG_QUEUE)] K_ALIGN(4);
static VOID *dynMesgqRegistry[RK_CONF_DYNAMIC_MESG_QUEUES_MAX];
static USHORT dynMesgqGeneration[RK_CONF_DYNAMIC_MESG_QUEUES_MAX];
#endif

#if ((RK_CONF_CALLOUT_TIMER == ON) && (RK_CONF_DYNAMIC_TIMERS_MAX > 0U))
#if (RK_CONF_DYNAMIC_TIMERS_MAX > 256U)
#error "RK_CONF_DYNAMIC_TIMERS_MAX exceeds opaque handle slot capacity"
#endif
static RK_MEM_PARTITION dynTimerPart;
static ULONG dynTimerPool[RK_CONF_DYNAMIC_TIMERS_MAX]
                          [RK_TYPE_WORD_COUNT(RK_TIMER)] K_ALIGN(4);
static VOID *dynTimerRegistry[RK_CONF_DYNAMIC_TIMERS_MAX];
static USHORT dynTimerGeneration[RK_CONF_DYNAMIC_TIMERS_MAX];
#endif

#if ((RK_CONF_MRM == ON) && (RK_CONF_DYNAMIC_MRMS_MAX > 0U))
#if (RK_CONF_DYNAMIC_MRMS_MAX > 256U)
#error "RK_CONF_DYNAMIC_MRMS_MAX exceeds opaque handle slot capacity"
#endif
static RK_MEM_PARTITION dynMrmPart;
static ULONG dynMrmPool[RK_CONF_DYNAMIC_MRMS_MAX]
                        [RK_TYPE_WORD_COUNT(RK_MRM)] K_ALIGN(4);
static VOID *dynMrmRegistry[RK_CONF_DYNAMIC_MRMS_MAX];
static USHORT dynMrmGeneration[RK_CONF_DYNAMIC_MRMS_MAX];
#endif

#if (RK_CONF_DYNAMIC_SHARED_MEMS_MAX > 0U)
#if (RK_CONF_DYNAMIC_SHARED_MEMS_MAX > 256U)
#error "RK_CONF_DYNAMIC_SHARED_MEMS_MAX exceeds opaque handle slot capacity"
#endif
static RK_MEM_PARTITION dynSharedMemPart;
static ULONG dynSharedMemPool[RK_CONF_DYNAMIC_SHARED_MEMS_MAX]
                              [RK_TYPE_WORD_COUNT(RK_SHARED_MEM)] K_ALIGN(4);
static VOID *dynSharedMemRegistry[RK_CONF_DYNAMIC_SHARED_MEMS_MAX];
static USHORT dynSharedMemGeneration[RK_CONF_DYNAMIC_SHARED_MEMS_MAX];
#endif


/* helpers for checking Ready, Valid/Range, Init, Put, Get */
static RK_ERR kDynObjCheckReady_(VOID)
{
    if (dynObjPartitionsInit != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
#endif
        return (RK_ERR_OBJ_NOT_INIT);
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    return (RK_ERR_SUCCESS);
}

static RK_ERR kDynObjCheckCreateHandle_(RK_OBJ_HANDLE *const handlePtr)
{
    if (handlePtr == NULL)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    if (*handlePtr != RK_NULL_HANDLE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        return (RK_ERR_INVALID_PARAM);
    }

    return (RK_ERR_SUCCESS);
}

static RK_BOOL kDynObjPartValid_(RK_MEM_PARTITION const *const partPtr)
{
    return (kObjHeaderReady((partPtr != NULL) ? &partPtr->header : NULL,
                            RK_MEMALLOC_KOBJ_ID));
}

static RK_BOOL kDynObjPartOwnsBlock_(RK_MEM_PARTITION const *const partPtr,
                                     VOID const *const blockPtr,
                                     ULONG const minSize)
{
    if ((kDynObjPartValid_(partPtr) == RK_FALSE) || (blockPtr == NULL) ||
        (partPtr->blkSize < minSize))
    {
        return (RK_FALSE);
    }

    BYTE const *const poolStartPtr = partPtr->poolPtr;
    BYTE const *const poolEndPtr =
        poolStartPtr + (partPtr->blkSize * partPtr->nMaxBlocks);
    BYTE const *const blockBytePtr = (BYTE const *)blockPtr;
    if ((blockBytePtr < poolStartPtr) || (blockBytePtr >= poolEndPtr))
    {
        return (RK_FALSE);
    }

    ULONG const diff = (ULONG)(blockBytePtr - poolStartPtr);
    return (((diff % partPtr->blkSize) == 0UL) ? RK_TRUE : RK_FALSE);
}

static RK_ERR kDynObjClassByType_(RK_DYN_OBJ_TYPE const objType,
                                  RK_DYN_OBJ_CLASS_ *const classPtr)
{
    if (classPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    switch (objType)
    {
#if ((RK_CONF_SEMAPHORE == ON) && (RK_CONF_DYNAMIC_SEMAPHORES_MAX > 0U))
        case RK_DYN_OBJ_TYPE_SEMAPHORE:
            classPtr->partPtr = &dynSemaPart;
            classPtr->registryPtr = dynSemaRegistry;
            classPtr->generationPtr = dynSemaGeneration;
            classPtr->maxObjects = RK_CONF_DYNAMIC_SEMAPHORES_MAX;
            classPtr->objSize = sizeof(RK_SEMAPHORE);
            classPtr->objID = RK_SEMAPHORE_KOBJ_ID;
            return (RK_ERR_SUCCESS);
#endif

#if ((RK_CONF_MUTEX == ON) && (RK_CONF_DYNAMIC_MUTEXES_MAX > 0U))
        case RK_DYN_OBJ_TYPE_MUTEX:
            classPtr->partPtr = &dynMutexPart;
            classPtr->registryPtr = dynMutexRegistry;
            classPtr->generationPtr = dynMutexGeneration;
            classPtr->maxObjects = RK_CONF_DYNAMIC_MUTEXES_MAX;
            classPtr->objSize = sizeof(RK_MUTEX);
            classPtr->objID = RK_MUTEX_KOBJ_ID;
            return (RK_ERR_SUCCESS);
#endif

#if ((RK_CONF_SLEEP_QUEUE == ON) && (RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX > 0U))
        case RK_DYN_OBJ_TYPE_SLEEP_QUEUE:
            classPtr->partPtr = &dynSleepqPart;
            classPtr->registryPtr = dynSleepqRegistry;
            classPtr->generationPtr = dynSleepqGeneration;
            classPtr->maxObjects = RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX;
            classPtr->objSize = sizeof(RK_SLEEP_QUEUE);
            classPtr->objID = RK_SLEEPQ_KOBJ_ID;
            return (RK_ERR_SUCCESS);
#endif

#if ((RK_CONF_MESG_QUEUE == ON) && (RK_CONF_DYNAMIC_MESG_QUEUES_MAX > 0U))
        case RK_DYN_OBJ_TYPE_MESG_QUEUE:
            classPtr->partPtr = &dynMesgqPart;
            classPtr->registryPtr = dynMesgqRegistry;
            classPtr->generationPtr = dynMesgqGeneration;
            classPtr->maxObjects = RK_CONF_DYNAMIC_MESG_QUEUES_MAX;
            classPtr->objSize = sizeof(RK_MESG_QUEUE);
            classPtr->objID = RK_MESGQQUEUE_KOBJ_ID;
            return (RK_ERR_SUCCESS);
#endif

#if ((RK_CONF_CALLOUT_TIMER == ON) && (RK_CONF_DYNAMIC_TIMERS_MAX > 0U))
        case RK_DYN_OBJ_TYPE_TIMER:
            classPtr->partPtr = &dynTimerPart;
            classPtr->registryPtr = dynTimerRegistry;
            classPtr->generationPtr = dynTimerGeneration;
            classPtr->maxObjects = RK_CONF_DYNAMIC_TIMERS_MAX;
            classPtr->objSize = sizeof(RK_TIMER);
            classPtr->objID = RK_TIMER_KOBJ_ID;
            return (RK_ERR_SUCCESS);
#endif

#if ((RK_CONF_MRM == ON) && (RK_CONF_DYNAMIC_MRMS_MAX > 0U))
        case RK_DYN_OBJ_TYPE_MRM:
            classPtr->partPtr = &dynMrmPart;
            classPtr->registryPtr = dynMrmRegistry;
            classPtr->generationPtr = dynMrmGeneration;
            classPtr->maxObjects = RK_CONF_DYNAMIC_MRMS_MAX;
            classPtr->objSize = sizeof(RK_MRM);
            classPtr->objID = RK_MRM_KOBJ_ID;
            return (RK_ERR_SUCCESS);
#endif

#if (RK_CONF_DYNAMIC_SHARED_MEMS_MAX > 0U)
        case RK_DYN_OBJ_TYPE_SHARED_MEM:
            classPtr->partPtr = &dynSharedMemPart;
            classPtr->registryPtr = dynSharedMemRegistry;
            classPtr->generationPtr = dynSharedMemGeneration;
            classPtr->maxObjects = RK_CONF_DYNAMIC_SHARED_MEMS_MAX;
            classPtr->objSize = sizeof(RK_SHARED_MEM);
            classPtr->objID = RK_SHARED_MEM_KOBJ_ID;
            return (RK_ERR_SUCCESS);
#endif

        default:
            break;
    }

    return (RK_ERR_INVALID_OBJ);
}

/*
 * Convert an object pointer back to a fixed-pool slot number. This is used only
 * after proving the pointer lies inside the partition and exactly on a block
 * boundary, so the slot can be trusted for registry indexing.
 */
static RK_BOOL kDynObjPartSlot_(RK_MEM_PARTITION const *const partPtr,
                                VOID const *const blockPtr,
                                ULONG const minSize,
                                ULONG *const slotPtr)
{
    if (kDynObjPartOwnsBlock_(partPtr, blockPtr, minSize) == RK_FALSE)
    {
        return (RK_FALSE);
    }

    BYTE const *const poolStartPtr = partPtr->poolPtr;
    BYTE const *const blockBytePtr = (BYTE const *)blockPtr;
    ULONG const diff = (ULONG)(blockBytePtr - poolStartPtr);
    ULONG const slot = diff / partPtr->blkSize;

    if (slot >= partPtr->nMaxBlocks)
    {
        return (RK_FALSE);
    }

    if (slotPtr != NULL)
    {
        *slotPtr = slot;
    }

    return (RK_TRUE);
}

/*
 * Advance the per-slot generation on destroy. Generation zero is skipped so a
 * freshly zeroed handle cell, or an accidentally forged zero generation, never
 * aliases a valid live object.
 */
static VOID kDynObjGenerationBump_(RK_DYN_OBJ_CLASS_ const *const classPtr,
                                   ULONG const slot)
{
    if ((classPtr == NULL) || (slot >= classPtr->maxObjects))
    {
        return;
    }

    classPtr->generationPtr[slot]++;
    if (classPtr->generationPtr[slot] == 0U)
    {
        classPtr->generationPtr[slot] = 1U;
    }
}

/*
 * Build the public opaque handle. The returned value is a tagged token, not an
 * address: tag identifies it as an RK handle, type selects the object family,
 * generation rejects stale handles, and slot selects the fixed-pool entry.
 */
static RK_HANDLE kDynObjHandleEncode_(RK_DYN_OBJ_TYPE const objType,
                                      USHORT const generation,
                                      ULONG const slot)
{
    ULONG const rawHandle =
        RK_HANDLE_TAG | RK_HANDLE_TYPE_FIELD((ULONG)objType) |
        (((ULONG)generation & RK_HANDLE_GENERATION_MASK)
            << RK_HANDLE_GENERATION_SHIFT) |
        (slot & RK_HANDLE_SLOT_MASK);

    return ((RK_HANDLE)rawHandle);
}

/*
 * Decode only the type/slot/generation fields from a public handle. This does
 * not prove liveness; kDynObjResolveDecoded_() still checks generation,
 * registry contents, pool membership and object header before returning memory.
 */
static RK_BOOL kDynObjHandleDecode_(RK_DYN_OBJ_TYPE const objType,
                                    RK_HANDLE const handle,
                                    RK_DYN_OBJ_CLASS_ const *const classPtr,
                                    ULONG *const slotPtr,
                                    USHORT *const generationPtr)
{
    ULONG const rawHandle = (ULONG)handle;

    if (((rawHandle & RK_HANDLE_TAG_MASK) != RK_HANDLE_TAG) ||
        ((rawHandle & RK_HANDLE_TYPE_MASK) !=
         RK_HANDLE_TYPE_FIELD((ULONG)objType)))
    {
        return (RK_FALSE);
    }

    ULONG const slot = rawHandle & RK_HANDLE_SLOT_MASK;
    if ((classPtr == NULL) || (slot >= classPtr->maxObjects))
    {
        return (RK_FALSE);
    }

    if (slotPtr != NULL)
    {
        *slotPtr = slot;
    }
    if (generationPtr != NULL)
    {
        *generationPtr =
            (USHORT)((rawHandle >> RK_HANDLE_GENERATION_SHIFT) &
                     RK_HANDLE_GENERATION_MASK);
    }

    return (RK_TRUE);
}

/*
 * Resolve a decoded handle to object memory. Stale generation, empty registry
 * slot, wrong partition or wrong object header all fail before the caller sees
 * the kernel object pointer.
 */
static RK_ERR kDynObjResolveDecoded_(RK_DYN_OBJ_CLASS_ const *const classPtr,
                                     ULONG const slot,
                                     USHORT const generation,
                                     VOID **const objPPtr)
{
    if (objPPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    *objPPtr = NULL;

    if ((classPtr == NULL) || (slot >= classPtr->maxObjects) ||
        (classPtr->generationPtr[slot] != generation))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    VOID *const objPtr = classPtr->registryPtr[slot];
    if ((objPtr == NULL) ||
        (kDynObjPartOwnsBlock_(classPtr->partPtr, objPtr,
                               classPtr->objSize) == RK_FALSE) ||
        (((RK_KOBJ const *)objPtr)->objID != classPtr->objID))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    *objPPtr = objPtr;
    return (RK_ERR_SUCCESS);
}

RK_BOOL kDynObjHandleIsEncoded(RK_HANDLE const handle)
{
    ULONG const rawHandle = (ULONG)handle;

    return (((rawHandle & RK_HANDLE_TAG_MASK) == RK_HANDLE_TAG)
                ? RK_TRUE
                : RK_FALSE);
}

/*
 * Publish a just-initialised pool object as a handle. Precondition: objPtr is a
 * live object from the correct dynamic partition and the slot is not already
 * registered. Postcondition: handlePPtr receives the only public token for
 * this generation of the slot.
 */
RK_ERR kDynObjPublishHandle(RK_DYN_OBJ_TYPE const objType,
                            VOID *const objPtr,
                            RK_HANDLE *const handlePPtr)
{
    if ((objPtr == NULL) || (handlePPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }
    *handlePPtr = RK_NULL_HANDLE;

    RK_ERR err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    RK_DYN_OBJ_CLASS_ classInfo;
    err = kDynObjClassByType_(objType, &classInfo);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    ULONG slot = 0UL;
    if ((kDynObjPartSlot_(classInfo.partPtr, objPtr, classInfo.objSize,
                          &slot) == RK_FALSE) ||
        (((RK_KOBJ const *)objPtr)->objID != classInfo.objID))
    {
        return (RK_ERR_INVALID_OBJ);
    }

    RK_CR_AREA
    RK_CR_ENTER
    if (classInfo.registryPtr[slot] != NULL)
    {
        RK_CR_EXIT
        return (RK_ERR_OBJ_DOUBLE_INIT);
    }

    classInfo.registryPtr[slot] = objPtr;
    *handlePPtr =
        kDynObjHandleEncode_(objType, classInfo.generationPtr[slot], slot);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

/*
 * Resolve either current encoded handles or legacy raw object pointers. Raw
 * pointers are accepted only when they are not stale aliases of a dynamic pool
 * slot whose registry points somewhere else.
 */
RK_ERR kDynObjResolveHandle(RK_DYN_OBJ_TYPE const objType,
                            RK_HANDLE const handle,
                            VOID **const objPPtr)
{
    if ((handle == RK_NULL_HANDLE) || (objPPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }
    *objPPtr = NULL;

    RK_DYN_OBJ_CLASS_ classInfo;
    RK_ERR err = kDynObjClassByType_(objType, &classInfo);
    if (err != RK_ERR_SUCCESS)
    {
        *objPPtr = (VOID *)(UINTPTR)handle;
        return (RK_ERR_SUCCESS);
    }

    ULONG slot = 0UL;
    USHORT generation = 0U;
    if (kDynObjHandleDecode_(objType, handle, &classInfo, &slot,
                             &generation) == RK_TRUE)
    {
        return (kDynObjResolveDecoded_(&classInfo, slot, generation,
                                       objPPtr));
    }
    if (kDynObjHandleIsEncoded(handle) == RK_TRUE)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    VOID *const rawPtr = (VOID *)(UINTPTR)handle;
    if (kDynObjPartSlot_(classInfo.partPtr, rawPtr, classInfo.objSize,
                         &slot) == RK_TRUE)
    {
        if (classInfo.registryPtr[slot] != rawPtr)
        {
            return (RK_ERR_INVALID_OBJ);
        }
    }

    *objPPtr = rawPtr;
    return (RK_ERR_SUCCESS);
}

/*
 * Resolve a generic handle without the caller knowing the concrete object
 * family. Encoded handles are dispatched by their type field; non-encoded
 * pointers pass through for boot/internal code paths that still operate on raw
 * object addresses.
 */
RK_ERR kDynObjResolveAnyHandle(RK_HANDLE const handle,
                               VOID **const objPPtr)
{
    if ((handle == RK_NULL_HANDLE) || (objPPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }
    *objPPtr = NULL;

    if (kDynObjHandleIsEncoded(handle) == RK_FALSE)
    {
        *objPPtr = (VOID *)(UINTPTR)handle;
        return (RK_ERR_SUCCESS);
    }

    ULONG const rawHandle = (ULONG)handle;
    ULONG const kind =
        (rawHandle & RK_HANDLE_TYPE_MASK) >> 24U;

    switch (kind)
    {
        case RK_HANDLE_KIND_SEMAPHORE:
            return (kDynObjResolveHandle(RK_DYN_OBJ_TYPE_SEMAPHORE,
                                         handle, objPPtr));
        case RK_HANDLE_KIND_MUTEX:
            return (kDynObjResolveHandle(RK_DYN_OBJ_TYPE_MUTEX,
                                         handle, objPPtr));
        case RK_HANDLE_KIND_SLEEP_QUEUE:
            return (kDynObjResolveHandle(RK_DYN_OBJ_TYPE_SLEEP_QUEUE,
                                         handle, objPPtr));
        case RK_HANDLE_KIND_MESG_QUEUE:
            return (kDynObjResolveHandle(RK_DYN_OBJ_TYPE_MESG_QUEUE,
                                         handle, objPPtr));
        case RK_HANDLE_KIND_TIMER:
            return (kDynObjResolveHandle(RK_DYN_OBJ_TYPE_TIMER,
                                         handle, objPPtr));
        case RK_HANDLE_KIND_MRM:
            return (kDynObjResolveHandle(RK_DYN_OBJ_TYPE_MRM,
                                         handle, objPPtr));
        case RK_HANDLE_KIND_SHARED_MEM:
            return (kDynObjResolveHandle(RK_DYN_OBJ_TYPE_SHARED_MEM,
                                         handle, objPPtr));
        default:
            return (RK_ERR_INVALID_OBJ);
    }
}

/*
 * Resolve only objects that are owned by a dynamic object partition. This is
 * used by destroy paths so an arbitrary boot/static object cannot be returned
 * to a dynamic pool by mistake.
 */
RK_ERR kDynObjResolveDynamicHandle(RK_DYN_OBJ_TYPE const objType,
                                   RK_HANDLE const handle,
                                   VOID **const objPPtr)
{
    if ((handle == RK_NULL_HANDLE) || (objPPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }
    *objPPtr = NULL;

    RK_ERR err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    RK_DYN_OBJ_CLASS_ classInfo;
    err = kDynObjClassByType_(objType, &classInfo);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    ULONG slot = 0UL;
    USHORT generation = 0U;
    if (kDynObjHandleDecode_(objType, handle, &classInfo, &slot,
                             &generation) == RK_TRUE)
    {
        return (kDynObjResolveDecoded_(&classInfo, slot, generation,
                                       objPPtr));
    }
    if (kDynObjHandleIsEncoded(handle) == RK_TRUE)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    VOID *const rawPtr = (VOID *)(UINTPTR)handle;
    if (kDynObjPartSlot_(classInfo.partPtr, rawPtr, classInfo.objSize,
                         &slot) == RK_FALSE)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if (classInfo.registryPtr[slot] != rawPtr)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    *objPPtr = rawPtr;
    return (RK_ERR_SUCCESS);
}

/*
 * First half of destroy: remove the registry entry and advance generation
 * while still inside a critical section. Once this returns, outstanding handles
 * for the old object must fail even if the memory contents have not yet been
 * cleared.
 */
RK_ERR kDynObjInvalidateHandle(RK_DYN_OBJ_TYPE const objType,
                               RK_HANDLE const handle)
{
    if (handle == RK_NULL_HANDLE)
    {
        return (RK_ERR_OBJ_NULL);
    }

    RK_DYN_OBJ_CLASS_ classInfo;
    RK_ERR err = kDynObjClassByType_(objType, &classInfo);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    VOID *objPtr = NULL;
    err = kDynObjResolveDynamicHandle(objType, handle, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    ULONG slot = 0UL;
    if (kDynObjPartSlot_(classInfo.partPtr, objPtr, classInfo.objSize,
                         &slot) == RK_FALSE)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    RK_CR_AREA
    RK_CR_ENTER
    if (classInfo.registryPtr[slot] != objPtr)
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    classInfo.registryPtr[slot] = NULL;
    kDynObjGenerationBump_(&classInfo, slot);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

static RK_ERR kDynObjScopeAttrErr_(RK_OBJ_ATTR const *const attrPtr)
{
    if (attrPtr == NULL)
    {
        return (RK_ERR_SUCCESS);
    }

    if (attrPtr->scope == RK_SCOPE_KERNEL_GLOBAL)
    {
        return ((attrPtr->modulePtr == NULL) ? RK_ERR_SUCCESS
                                             : RK_ERR_INVALID_PARAM);
    }

    if (attrPtr->scope == RK_SCOPE_MODULE_LOCAL)
    {
        if (attrPtr->modulePtr == NULL)
        {
            return (RK_ERR_INVALID_PARAM);
        }

        return ((attrPtr->modulePtr->init == RK_TRUE) ? RK_ERR_SUCCESS
                                                      : RK_ERR_OBJ_NOT_INIT);
    }

    return (RK_ERR_INVALID_PARAM);
}

static RK_ERR kDynObjApplyScope_(RK_KOBJ *const headerPtr,
                                 RK_OBJ_ATTR const *const attrPtr)
{
    if (attrPtr == NULL)
    {
        if (RK_gRunPtr == NULL)
        {
            RK_ERR const err = kApplicationModuleEnsureInit();
            if (err != RK_ERR_SUCCESS)
            {
                return (err);
            }

            return (kObjHeaderScopeSet(headerPtr, RK_SCOPE_MODULE_LOCAL,
                                       kApplicationModuleGet()));
        }

        return (RK_ERR_SUCCESS);
    }

    return (kObjHeaderScopeSet(headerPtr, attrPtr->scope, attrPtr->modulePtr));
}

#if (RK_DYN_OBJ_HAS_ANY_POOL)
/* Initialise each dynamic object partition through the boot-only raw init path. */
static RK_ERR kDynObjInitPart_(RK_MEM_PARTITION *const partPtr,
                               VOID *const poolPtr,
                               ULONG const objSize,
                               ULONG const maxObjects,
                               CHAR const *const namePtr)
{
    if (partPtr->init == RK_TRUE)
    {
        return (RK_ERR_SUCCESS);
    }

    kKernelConstructionEnter();
    RK_ERR err = kMemPartitionInitGlobalScope(partPtr, poolPtr, objSize,
                                              maxObjects);
    kKernelConstructionExit();
    if (err == RK_ERR_SUCCESS)
    {
        kTraceNameObject(partPtr, namePtr);
    }
    return (err);
}

/* Clear object contents before returning its block to the fixed partition. */
static RK_ERR kDynObjReleaseBlock_(RK_MEM_PARTITION *const partPtr,
                                   VOID *const objPtr,
                                   ULONG const objSize)
{
    RK_MEMSET(objPtr, 0, objSize);

    return (kMemPartitionFree(partPtr, objPtr));
}
#endif

#if (RK_DYN_OBJ_HAS_STATEFUL_DESTROY)
static RK_ERR kDynObjInvalidState_(VOID)
{
#if (RK_CONF_ERR_CHECK == ON)
    K_ERR_HANDLER(RK_FAULT_TASK_INVALID_STATE);
#endif
    return (RK_ERR_TASK_INVALID_ST);
}
#endif

/* Initialise every configured kernel-owned dynamic object partition. */
RK_ERR kObjPartitionsInit(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_OBJ_PARTITIONS_INIT,
                                        0UL, 0UL, 0UL, 0UL));
    }

    if (kIsISR())
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
#endif
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    RK_ERR err = kKernelRawInitGuard();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = RK_ERR_SUCCESS;

#if ((RK_CONF_SEMAPHORE == ON) && (RK_CONF_DYNAMIC_SEMAPHORES_MAX > 0U))
    err = kDynObjInitPart_(&dynSemaPart, dynSemaPool, sizeof(RK_SEMAPHORE),
                           RK_CONF_DYNAMIC_SEMAPHORES_MAX, "DynSem");
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
#endif

#if ((RK_CONF_MUTEX == ON) && (RK_CONF_DYNAMIC_MUTEXES_MAX > 0U))
    err = kDynObjInitPart_(&dynMutexPart, dynMutexPool, sizeof(RK_MUTEX),
                           RK_CONF_DYNAMIC_MUTEXES_MAX, "DynMut");
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
#endif

#if ((RK_CONF_SLEEP_QUEUE == ON) && (RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX > 0U))
    err = kDynObjInitPart_(&dynSleepqPart, dynSleepqPool,
                           sizeof(RK_SLEEP_QUEUE),
                           RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX, "DynSlp");
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
#endif

#if ((RK_CONF_MESG_QUEUE == ON) && (RK_CONF_DYNAMIC_MESG_QUEUES_MAX > 0U))
    err = kDynObjInitPart_(&dynMesgqPart, dynMesgqPool,
                           sizeof(RK_MESG_QUEUE),
                           RK_CONF_DYNAMIC_MESG_QUEUES_MAX, "DynMsg");
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
#endif

#if ((RK_CONF_CALLOUT_TIMER == ON) && (RK_CONF_DYNAMIC_TIMERS_MAX > 0U))
    err = kDynObjInitPart_(&dynTimerPart, dynTimerPool, sizeof(RK_TIMER),
                           RK_CONF_DYNAMIC_TIMERS_MAX, "DynTmr");
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
#endif

#if ((RK_CONF_MRM == ON) && (RK_CONF_DYNAMIC_MRMS_MAX > 0U))
    err = kDynObjInitPart_(&dynMrmPart, dynMrmPool, sizeof(RK_MRM),
                           RK_CONF_DYNAMIC_MRMS_MAX, "DynMRM");
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
#endif

#if (RK_CONF_DYNAMIC_SHARED_MEMS_MAX > 0U)
    err = kDynObjInitPart_(&dynSharedMemPart, dynSharedMemPool,
                           sizeof(RK_SHARED_MEM),
                           RK_CONF_DYNAMIC_SHARED_MEMS_MAX, "DynShm");
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
#endif

    dynObjPartitionsInit = RK_TRUE;
    return (RK_ERR_SUCCESS);
}

/* Runtime create/destroy methods per object. Created object handles are
 * encoded type/slot/generation tokens resolved through the registries above.
 * Public Create APIs require an RK_NULL_HANDLE input handle and allocate
 * object storage from the fixed partition pool for that object family.
 */

RK_ERR kSharedMemCreate(RK_SHARED_MEM_HANDLE *const sharedMemHandlePtr,
                        VOID *const regionBasePtr,
                        ULONG const regionBytes)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SHARED_MEM_CREATE,
            (ULONG)(UINTPTR)sharedMemHandlePtr,
            (ULONG)(UINTPTR)regionBasePtr, regionBytes, 0UL));
    }

    RK_ERR err = kDynObjCheckCreateHandle_(sharedMemHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    if (kMpuLayoutIsFinalized() == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PHASE);
#endif
        return (RK_ERR_INVALID_PHASE);
    }

#if (RK_CONF_DYNAMIC_SHARED_MEMS_MAX > 0U)
    RK_SHARED_MEM *const sharedMemPtr =
        (RK_SHARED_MEM *)kMemPartitionAlloc(&dynSharedMemPart);
    if (sharedMemPtr == NULL)
    {
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_MEMSET(sharedMemPtr, 0, sizeof(RK_SHARED_MEM));
    kKernelConstructionEnter();
    err = kSharedMemInit(sharedMemPtr, regionBasePtr, regionBytes);
    kKernelConstructionExit();
    if (err != RK_ERR_SUCCESS)
    {
        RK_MEMSET(sharedMemPtr, 0, sizeof(RK_SHARED_MEM));
        kMemPartitionFree(&dynSharedMemPart, sharedMemPtr);
        return (err);
    }

    RK_HANDLE handle = RK_NULL_HANDLE;
    err = kDynObjPublishHandle(RK_DYN_OBJ_TYPE_SHARED_MEM, sharedMemPtr,
                               &handle);
    if (err != RK_ERR_SUCCESS)
    {
        kMpuSharedRegionMemoryRelease(&sharedMemPtr->region);
        kTraceUnregisterObject(sharedMemPtr);
        RK_MEMSET(sharedMemPtr, 0, sizeof(RK_SHARED_MEM));
        kMemPartitionFree(&dynSharedMemPart, sharedMemPtr);
        return (err);
    }

    *sharedMemHandlePtr = (RK_SHARED_MEM_HANDLE)handle;
    return (RK_ERR_SUCCESS);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}

RK_ERR kSharedMemDestroy(RK_SHARED_MEM_HANDLE *const sharedMemHandlePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SHARED_MEM_DESTROY,
            (ULONG)(UINTPTR)sharedMemHandlePtr, 0UL, 0UL, 0UL));
    }

    if ((sharedMemHandlePtr == NULL) ||
        (*sharedMemHandlePtr == RK_NULL_HANDLE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    if (kMpuLayoutIsFinalized() == RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PHASE);
#endif
        return (RK_ERR_INVALID_PHASE);
    }

#if (RK_CONF_DYNAMIC_SHARED_MEMS_MAX > 0U)
    RK_CR_AREA
    RK_CR_ENTER

    VOID *objPtr = NULL;
    err = kDynObjResolveDynamicHandle(RK_DYN_OBJ_TYPE_SHARED_MEM,
                                      *sharedMemHandlePtr, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    RK_SHARED_MEM *const sharedMemPtr = (RK_SHARED_MEM *)objPtr;

    if (kObjHeaderReady(&sharedMemPtr->header,
                        RK_SHARED_MEM_KOBJ_ID) != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    if (sharedMemPtr->attachCount != 0UL)
    {
        RK_CR_EXIT
        return (kDynObjInvalidState_());
    }

    err = kDynObjInvalidateHandle(RK_DYN_OBJ_TYPE_SHARED_MEM,
                                  *sharedMemHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    kMpuSharedRegionMemoryRelease(&sharedMemPtr->region);
    kTraceRecordObject(sharedMemPtr, RK_TRACE_OP_FREE, RK_ERR_SUCCESS, 0UL);
    kTraceUnregisterObject(sharedMemPtr);
    err = kDynObjReleaseBlock_(&dynSharedMemPart, sharedMemPtr,
                               sizeof(RK_SHARED_MEM));
    if (err == RK_ERR_SUCCESS)
    {
        *sharedMemHandlePtr = RK_NULL_HANDLE;
    }
    RK_CR_EXIT
    return (err);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}

#if (RK_CONF_SEMAPHORE == ON)
static RK_ERR kSemaphoreCreateWithAttr_(
    RK_SEMAPHORE_HANDLE *const semaHandlePtr,
    UINT const initValue,
    UINT const maxValue,
    RK_OBJ_ATTR const *const attrPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    RK_ERR err = kDynObjScopeAttrErr_(attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckCreateHandle_(semaHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_SEMAPHORES_MAX > 0U)
    RK_SEMAPHORE *const semaPtr =
        (RK_SEMAPHORE *)kMemPartitionAlloc(&dynSemaPart);
    if (semaPtr == NULL)
    {
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_MEMSET(semaPtr, 0, sizeof(RK_SEMAPHORE));
    kKernelConstructionEnter();
    err = kSemaphoreInit(semaPtr, initValue, maxValue);
    kKernelConstructionExit();
    if (err != RK_ERR_SUCCESS)
    {
        RK_MEMSET(semaPtr, 0, sizeof(RK_SEMAPHORE));
        kMemPartitionFree(&dynSemaPart, semaPtr);
        return (err);
    }

    err = kDynObjApplyScope_(&semaPtr->header, attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        kTraceUnregisterObject(semaPtr);
        RK_MEMSET(semaPtr, 0, sizeof(RK_SEMAPHORE));
        kMemPartitionFree(&dynSemaPart, semaPtr);
        return (err);
    }

    RK_HANDLE handle = RK_NULL_HANDLE;
    err = kDynObjPublishHandle(RK_DYN_OBJ_TYPE_SEMAPHORE, semaPtr,
                               &handle);
    if (err != RK_ERR_SUCCESS)
    {
        kTraceUnregisterObject(semaPtr);
        RK_MEMSET(semaPtr, 0, sizeof(RK_SEMAPHORE));
        kMemPartitionFree(&dynSemaPart, semaPtr);
        return (err);
    }

    *semaHandlePtr = (RK_SEMAPHORE_HANDLE)handle;
    return (RK_ERR_SUCCESS);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}

RK_ERR kSemaphoreCreate(RK_SEMAPHORE_HANDLE *const semaHandlePtr,
                        UINT const initValue,
                        UINT const maxValue)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SEMAPHORE_CREATE,
            (ULONG)(UINTPTR)semaHandlePtr, (ULONG)initValue,
            (ULONG)maxValue, 0UL));
    }

    return (kSemaphoreCreateWithAttr_(semaHandlePtr, initValue, maxValue,
                                      NULL));
}

RK_ERR kSemaphoreCreateGlobalScope(RK_SEMAPHORE_HANDLE *const semaHandlePtr,
                                   UINT const initValue,
                                   UINT const maxValue)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_KERNEL_GLOBAL, NULL };
    return (kSemaphoreCreateWithAttr_(semaHandlePtr, initValue, maxValue,
                                      &attr));
}

RK_ERR kSemaphoreCreateModuleScope(RK_SEMAPHORE_HANDLE *const semaHandlePtr,
                                   UINT const initValue,
                                   UINT const maxValue,
                                   RK_MODULE *const modulePtr)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_MODULE_LOCAL, modulePtr };
    return (kSemaphoreCreateWithAttr_(semaHandlePtr, initValue, maxValue,
                                      &attr));
}

RK_ERR kSemaphoreDestroy(RK_SEMAPHORE_HANDLE *const semaHandlePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SEMAPHORE_DESTROY,
            (ULONG)(UINTPTR)semaHandlePtr, 0UL, 0UL, 0UL));
    }

    if ((semaHandlePtr == NULL) || (*semaHandlePtr == RK_NULL_HANDLE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_SEMAPHORES_MAX > 0U)
    RK_CR_AREA
    RK_CR_ENTER

    VOID *objPtr = NULL;
    err = kDynObjResolveDynamicHandle(RK_DYN_OBJ_TYPE_SEMAPHORE,
                                      *semaHandlePtr, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    RK_SEMAPHORE *const semaPtr = (RK_SEMAPHORE *)objPtr;

    if (kObjHeaderReady(&semaPtr->header, RK_SEMAPHORE_KOBJ_ID) != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    err = kObjHeaderModuleLocalAccessErr(
        &semaPtr->header, (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr : NULL);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    if (semaPtr->waitingQueue.size > 0UL)
    {
        RK_CR_EXIT
        return (kDynObjInvalidState_());
    }

    err = kDynObjInvalidateHandle(RK_DYN_OBJ_TYPE_SEMAPHORE,
                                  *semaHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    kTraceRecordObject(semaPtr, RK_TRACE_OP_FREE, RK_ERR_SUCCESS,
                       semaPtr->value);
    kTraceUnregisterObject(semaPtr);
    err = kDynObjReleaseBlock_(&dynSemaPart, semaPtr,
                               sizeof(RK_SEMAPHORE));
    if (err == RK_ERR_SUCCESS)
    {
        *semaHandlePtr = RK_NULL_HANDLE;
    }
    RK_CR_EXIT
    return (err);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}
#endif

#if (RK_CONF_MUTEX == ON)
static RK_ERR kMutexCreateWithAttr_(RK_MUTEX_HANDLE *const mutexHandlePtr,
                                    UINT const protocol,
                                    RK_OBJ_ATTR const *const attrPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    RK_ERR err = kDynObjScopeAttrErr_(attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckCreateHandle_(mutexHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_MUTEXES_MAX > 0U)
    RK_MUTEX *const mutexPtr =
        (RK_MUTEX *)kMemPartitionAlloc(&dynMutexPart);
    if (mutexPtr == NULL)
    {
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_MEMSET(mutexPtr, 0, sizeof(RK_MUTEX));
    kKernelConstructionEnter();
    err = kMutexInit(mutexPtr, protocol);
    kKernelConstructionExit();
    if (err != RK_ERR_SUCCESS)
    {
        RK_MEMSET(mutexPtr, 0, sizeof(RK_MUTEX));
        kMemPartitionFree(&dynMutexPart, mutexPtr);
        return (err);
    }

    err = kDynObjApplyScope_(&mutexPtr->header, attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        kTraceUnregisterObject(mutexPtr);
        RK_MEMSET(mutexPtr, 0, sizeof(RK_MUTEX));
        kMemPartitionFree(&dynMutexPart, mutexPtr);
        return (err);
    }

    RK_HANDLE handle = RK_NULL_HANDLE;
    err = kDynObjPublishHandle(RK_DYN_OBJ_TYPE_MUTEX, mutexPtr,
                               &handle);
    if (err != RK_ERR_SUCCESS)
    {
        kTraceUnregisterObject(mutexPtr);
        RK_MEMSET(mutexPtr, 0, sizeof(RK_MUTEX));
        kMemPartitionFree(&dynMutexPart, mutexPtr);
        return (err);
    }

    *mutexHandlePtr = (RK_MUTEX_HANDLE)handle;
    return (RK_ERR_SUCCESS);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}

RK_ERR kMutexCreate(RK_MUTEX_HANDLE *const mutexHandlePtr,
                    UINT const protocol)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MUTEX_CREATE, (ULONG)(UINTPTR)mutexHandlePtr,
            (ULONG)protocol, 0UL, 0UL));
    }

    return (kMutexCreateWithAttr_(mutexHandlePtr, protocol, NULL));
}

RK_ERR kMutexCreateGlobalScope(RK_MUTEX_HANDLE *const mutexHandlePtr,
                               UINT const protocol)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_KERNEL_GLOBAL, NULL };
    return (kMutexCreateWithAttr_(mutexHandlePtr, protocol, &attr));
}

RK_ERR kMutexCreateModuleScope(RK_MUTEX_HANDLE *const mutexHandlePtr,
                               UINT const protocol,
                               RK_MODULE *const modulePtr)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_MODULE_LOCAL, modulePtr };
    return (kMutexCreateWithAttr_(mutexHandlePtr, protocol, &attr));
}

RK_ERR kMutexDestroy(RK_MUTEX_HANDLE *const mutexHandlePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MUTEX_DESTROY, (ULONG)(UINTPTR)mutexHandlePtr,
            0UL, 0UL, 0UL));
    }

    if ((mutexHandlePtr == NULL) || (*mutexHandlePtr == RK_NULL_HANDLE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_MUTEXES_MAX > 0U)
    RK_CR_AREA
    RK_CR_ENTER

    VOID *objPtr = NULL;
    err = kDynObjResolveDynamicHandle(RK_DYN_OBJ_TYPE_MUTEX,
                                      *mutexHandlePtr, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    RK_MUTEX *const mutexPtr = (RK_MUTEX *)objPtr;

    if (kObjHeaderReady(&mutexPtr->header, RK_MUTEX_KOBJ_ID) != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    err = kObjHeaderModuleLocalAccessErr(
        &mutexPtr->header, (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr : NULL);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    if (mutexPtr->lock != RK_FALSE)
    {
        RK_CR_EXIT
        return (RK_ERR_MUTEX_LOCKED);
    }

    if ((mutexPtr->ownerPtr != NULL) || (mutexPtr->waitingQueue.size > 0UL))
    {
        RK_CR_EXIT
        return (kDynObjInvalidState_());
    }

    err = kDynObjInvalidateHandle(RK_DYN_OBJ_TYPE_MUTEX, *mutexHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    kTraceRecordObject(mutexPtr, RK_TRACE_OP_FREE, RK_ERR_SUCCESS, 0UL);
    kTraceUnregisterObject(mutexPtr);
    err = kDynObjReleaseBlock_(&dynMutexPart, mutexPtr,
                               sizeof(RK_MUTEX));
    if (err == RK_ERR_SUCCESS)
    {
        *mutexHandlePtr = RK_NULL_HANDLE;
    }
    RK_CR_EXIT
    return (err);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}
#endif

#if (RK_CONF_SLEEP_QUEUE == ON)
static RK_ERR kSleepQueueCreateWithAttr_(
    RK_SLEEP_QUEUE_HANDLE *const sleepqHandlePtr,
    RK_OBJ_ATTR const *const attrPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    RK_ERR err = kDynObjScopeAttrErr_(attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckCreateHandle_(sleepqHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX > 0U)
    RK_SLEEP_QUEUE *const sleepqPtr =
        (RK_SLEEP_QUEUE *)kMemPartitionAlloc(&dynSleepqPart);
    if (sleepqPtr == NULL)
    {
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_MEMSET(sleepqPtr, 0, sizeof(RK_SLEEP_QUEUE));
    kKernelConstructionEnter();
    err = kSleepQueueInit(sleepqPtr);
    kKernelConstructionExit();
    if (err != RK_ERR_SUCCESS)
    {
        RK_MEMSET(sleepqPtr, 0, sizeof(RK_SLEEP_QUEUE));
        kMemPartitionFree(&dynSleepqPart, sleepqPtr);
        return (err);
    }

    err = kDynObjApplyScope_(&sleepqPtr->header, attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        kTraceUnregisterObject(sleepqPtr);
        RK_MEMSET(sleepqPtr, 0, sizeof(RK_SLEEP_QUEUE));
        kMemPartitionFree(&dynSleepqPart, sleepqPtr);
        return (err);
    }

    RK_HANDLE handle = RK_NULL_HANDLE;
    err = kDynObjPublishHandle(RK_DYN_OBJ_TYPE_SLEEP_QUEUE, sleepqPtr,
                               &handle);
    if (err != RK_ERR_SUCCESS)
    {
        kTraceUnregisterObject(sleepqPtr);
        RK_MEMSET(sleepqPtr, 0, sizeof(RK_SLEEP_QUEUE));
        kMemPartitionFree(&dynSleepqPart, sleepqPtr);
        return (err);
    }

    *sleepqHandlePtr = (RK_SLEEP_QUEUE_HANDLE)handle;
    return (RK_ERR_SUCCESS);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}

RK_ERR kSleepQueueCreate(RK_SLEEP_QUEUE_HANDLE *const sleepqHandlePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SLEEP_QUEUE_CREATE,
            (ULONG)(UINTPTR)sleepqHandlePtr, 0UL, 0UL, 0UL));
    }

    return (kSleepQueueCreateWithAttr_(sleepqHandlePtr, NULL));
}

RK_ERR kSleepQueueCreateGlobalScope(
    RK_SLEEP_QUEUE_HANDLE *const sleepqHandlePtr)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_KERNEL_GLOBAL, NULL };
    return (kSleepQueueCreateWithAttr_(sleepqHandlePtr, &attr));
}

RK_ERR kSleepQueueCreateModuleScope(
    RK_SLEEP_QUEUE_HANDLE *const sleepqHandlePtr,
    RK_MODULE *const modulePtr)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_MODULE_LOCAL, modulePtr };
    return (kSleepQueueCreateWithAttr_(sleepqHandlePtr, &attr));
}

RK_ERR kSleepQueueDestroy(RK_SLEEP_QUEUE_HANDLE *const sleepqHandlePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SLEEP_QUEUE_DESTROY,
            (ULONG)(UINTPTR)sleepqHandlePtr, 0UL, 0UL, 0UL));
    }

    if ((sleepqHandlePtr == NULL) || (*sleepqHandlePtr == RK_NULL_HANDLE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX > 0U)
    RK_CR_AREA
    RK_CR_ENTER

    VOID *objPtr = NULL;
    err = kDynObjResolveDynamicHandle(RK_DYN_OBJ_TYPE_SLEEP_QUEUE,
                                      *sleepqHandlePtr, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    RK_SLEEP_QUEUE *const sleepqPtr = (RK_SLEEP_QUEUE *)objPtr;

    if (kObjHeaderReady(&sleepqPtr->header, RK_SLEEPQ_KOBJ_ID) != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    err = kObjHeaderModuleLocalAccessErr(
        &sleepqPtr->header,
        (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr : NULL);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    if (sleepqPtr->waitingQueue.size > 0UL)
    {
        RK_CR_EXIT
        return (kDynObjInvalidState_());
    }

    err = kDynObjInvalidateHandle(RK_DYN_OBJ_TYPE_SLEEP_QUEUE,
                                  *sleepqHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    kTraceRecordObject(sleepqPtr, RK_TRACE_OP_FREE, RK_ERR_SUCCESS, 0UL);
    kTraceUnregisterObject(sleepqPtr);
    err = kDynObjReleaseBlock_(&dynSleepqPart, sleepqPtr,
                               sizeof(RK_SLEEP_QUEUE));
    if (err == RK_ERR_SUCCESS)
    {
        *sleepqHandlePtr = RK_NULL_HANDLE;
    }
    RK_CR_EXIT
    return (err);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}
#endif

#if (RK_CONF_MESG_QUEUE == ON)
static RK_ERR kMesgQueueCreateWithAttr_(
    RK_MESG_QUEUE_HANDLE *const queueHandlePtr,
    VOID *const bufPtr,
    ULONG const mesgWords,
    ULONG const depth,
    RK_OBJ_ATTR const *const attrPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    RK_ERR err = kDynObjScopeAttrErr_(attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckCreateHandle_(queueHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_MESG_QUEUES_MAX > 0U)
    RK_MESG_QUEUE *const queuePtr =
        (RK_MESG_QUEUE *)kMemPartitionAlloc(&dynMesgqPart);
    if (queuePtr == NULL)
    {
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_MEMSET(queuePtr, 0, sizeof(RK_MESG_QUEUE));
    kKernelConstructionEnter();
    err = kMesgQueueInit(queuePtr, bufPtr, mesgWords, depth);
    kKernelConstructionExit();
    if (err != RK_ERR_SUCCESS)
    {
        RK_MEMSET(queuePtr, 0, sizeof(RK_MESG_QUEUE));
        kMemPartitionFree(&dynMesgqPart, queuePtr);
        return (err);
    }

    err = kDynObjApplyScope_(&queuePtr->header, attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        kTraceUnregisterObject(queuePtr);
        RK_MEMSET(queuePtr, 0, sizeof(RK_MESG_QUEUE));
        kMemPartitionFree(&dynMesgqPart, queuePtr);
        return (err);
    }

    RK_HANDLE handle = RK_NULL_HANDLE;
    err = kDynObjPublishHandle(RK_DYN_OBJ_TYPE_MESG_QUEUE, queuePtr,
                               &handle);
    if (err != RK_ERR_SUCCESS)
    {
        kTraceUnregisterObject(queuePtr);
        RK_MEMSET(queuePtr, 0, sizeof(RK_MESG_QUEUE));
        kMemPartitionFree(&dynMesgqPart, queuePtr);
        return (err);
    }

    *queueHandlePtr = (RK_MESG_QUEUE_HANDLE)handle;
    return (RK_ERR_SUCCESS);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}

RK_ERR kMesgQueueCreate(RK_MESG_QUEUE_HANDLE *const queueHandlePtr,
                        VOID *const bufPtr,
                        ULONG const mesgWords,
                        ULONG const depth)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_CREATE,
            (ULONG)(UINTPTR)queueHandlePtr, (ULONG)(UINTPTR)bufPtr,
            (ULONG)mesgWords, (ULONG)depth));
    }

    return (kMesgQueueCreateWithAttr_(queueHandlePtr, bufPtr, mesgWords,
                                      depth, NULL));
}

RK_ERR kMesgQueueCreateGlobalScope(
    RK_MESG_QUEUE_HANDLE *const queueHandlePtr,
    VOID *const bufPtr,
    ULONG const mesgWords,
    ULONG const depth)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_KERNEL_GLOBAL, NULL };
    return (kMesgQueueCreateWithAttr_(queueHandlePtr, bufPtr, mesgWords,
                                      depth, &attr));
}

RK_ERR kMesgQueueCreateModuleScope(
    RK_MESG_QUEUE_HANDLE *const queueHandlePtr,
    VOID *const bufPtr,
    ULONG const mesgWords,
    ULONG const depth,
    RK_MODULE *const modulePtr)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_MODULE_LOCAL, modulePtr };
    return (kMesgQueueCreateWithAttr_(queueHandlePtr, bufPtr, mesgWords,
                                      depth, &attr));
}

RK_ERR kMesgQueueDestroy(RK_MESG_QUEUE_HANDLE *const queueHandlePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_MESG_QUEUE_DESTROY,
            (ULONG)(UINTPTR)queueHandlePtr, 0UL, 0UL, 0UL));
    }

    if ((queueHandlePtr == NULL) || (*queueHandlePtr == RK_NULL_HANDLE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_MESG_QUEUES_MAX > 0U)
    RK_CR_AREA
    RK_CR_ENTER

    VOID *objPtr = NULL;
    err = kDynObjResolveDynamicHandle(RK_DYN_OBJ_TYPE_MESG_QUEUE,
                                      *queueHandlePtr, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    RK_MESG_QUEUE *const queuePtr = (RK_MESG_QUEUE *)objPtr;

    if (kObjHeaderReady(&queuePtr->header, RK_MESGQQUEUE_KOBJ_ID) != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    err = kObjHeaderModuleLocalAccessErr(
        &queuePtr->header,
        (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr : NULL);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    if ((queuePtr->waitingReceivers.size > 0UL) ||
        (queuePtr->waitingSenders.size > 0UL) ||
        (queuePtr->ringBuf.nFull > 0UL) ||
        (queuePtr->broadcastReceivers > 0UL))
    {
        RK_CR_EXIT
        return (kDynObjInvalidState_());
    }

    err = kDynObjInvalidateHandle(RK_DYN_OBJ_TYPE_MESG_QUEUE,
                                  *queueHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    kTraceRecordObject(queuePtr, RK_TRACE_OP_FREE, RK_ERR_SUCCESS, 0UL);
    kTraceUnregisterObject(queuePtr);
    err = kDynObjReleaseBlock_(&dynMesgqPart, queuePtr,
                               sizeof(RK_MESG_QUEUE));
    if (err == RK_ERR_SUCCESS)
    {
        *queueHandlePtr = RK_NULL_HANDLE;
    }
    RK_CR_EXIT
    return (err);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}
#endif

/* timers are specially useful to be dynamic if using one-shot */

#if (RK_CONF_CALLOUT_TIMER == ON)
static RK_ERR kTimerCreateWithAttr_(
    RK_TIMER_HANDLE *const timerHandlePtr,
    RK_TICK const phase,
    RK_TICK const countTicks,
    RK_TIMER_CALLOUT const funPtr,
    VOID *const argsPtr,
    RK_OPTION const reload,
    RK_OBJ_ATTR const *const attrPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    RK_ERR err = kDynObjScopeAttrErr_(attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckCreateHandle_(timerHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_TIMERS_MAX > 0U)
    RK_TIMER *const timerPtr =
        (RK_TIMER *)kMemPartitionAlloc(&dynTimerPart);
    if (timerPtr == NULL)
    {
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_MEMSET(timerPtr, 0, sizeof(RK_TIMER));
    kKernelConstructionEnter();
    err = kTimerInit(timerPtr, phase, countTicks, funPtr, argsPtr, reload);
    kKernelConstructionExit();
    if (err != RK_ERR_SUCCESS)
    {
        RK_MEMSET(timerPtr, 0, sizeof(RK_TIMER));
        kMemPartitionFree(&dynTimerPart, timerPtr);
        return (err);
    }

    err = kDynObjApplyScope_(&timerPtr->header, attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        if (kTimeoutNodeIsArmed(&timerPtr->timeoutNode) == RK_TRUE)
        {
            (VOID)kTimeoutNodeDisarm(&timerPtr->timeoutNode);
        }
        kTraceUnregisterObject(timerPtr);
        RK_MEMSET(timerPtr, 0, sizeof(RK_TIMER));
        kMemPartitionFree(&dynTimerPart, timerPtr);
        return (err);
    }

    RK_HANDLE handle = RK_NULL_HANDLE;
    err = kDynObjPublishHandle(RK_DYN_OBJ_TYPE_TIMER, timerPtr,
                               &handle);
    if (err != RK_ERR_SUCCESS)
    {
        if (kTimeoutNodeIsArmed(&timerPtr->timeoutNode) == RK_TRUE)
        {
            (VOID)kTimeoutNodeDisarm(&timerPtr->timeoutNode);
        }
        kTraceUnregisterObject(timerPtr);
        RK_MEMSET(timerPtr, 0, sizeof(RK_TIMER));
        kMemPartitionFree(&dynTimerPart, timerPtr);
        return (err);
    }

    *timerHandlePtr = (RK_TIMER_HANDLE)handle;
    return (RK_ERR_SUCCESS);
#else
    K_UNUSE(phase);
    K_UNUSE(countTicks);
    K_UNUSE(funPtr);
    K_UNUSE(argsPtr);
    K_UNUSE(reload);
    return (RK_ERR_BUFFER_EMPTY);
#endif
}

RK_ERR kTimerCreate(RK_TIMER_HANDLE *const timerHandlePtr,
                    RK_TICK const phase,
                    RK_TICK const countTicks,
                    RK_TIMER_CALLOUT const funPtr,
                    VOID *const argsPtr,
                    RK_OPTION const reload)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        RK_TIMER_CREATE_SYSCALL_ARGS syscallArgs;

        syscallArgs.timerHandlePtr = timerHandlePtr;
        syscallArgs.phase = phase;
        syscallArgs.countTicks = countTicks;
        syscallArgs.funPtr = funPtr;
        syscallArgs.argsPtr = argsPtr;
        syscallArgs.reload = reload;

        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TIMER_CREATE,
                                        (ULONG)(UINTPTR)&syscallArgs,
                                        0UL, 0UL, 0UL));
    }

    return (kTimerCreateWithAttr_(timerHandlePtr, phase, countTicks, funPtr,
                                  argsPtr, reload, NULL));
}

RK_ERR kTimerCreateGlobalScope(RK_TIMER_HANDLE *const timerHandlePtr,
                               RK_TICK const phase,
                               RK_TICK const countTicks,
                               RK_TIMER_CALLOUT const funPtr,
                               VOID *const argsPtr,
                               RK_OPTION const reload)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_KERNEL_GLOBAL, NULL };
    return (kTimerCreateWithAttr_(timerHandlePtr, phase, countTicks, funPtr,
                                  argsPtr, reload, &attr));
}

RK_ERR kTimerCreateModuleScope(RK_TIMER_HANDLE *const timerHandlePtr,
                               RK_TICK const phase,
                               RK_TICK const countTicks,
                               RK_TIMER_CALLOUT const funPtr,
                               VOID *const argsPtr,
                               RK_OPTION const reload,
                               RK_MODULE *const modulePtr)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_MODULE_LOCAL, modulePtr };
    return (kTimerCreateWithAttr_(timerHandlePtr, phase, countTicks, funPtr,
                                  argsPtr, reload, &attr));
}

RK_ERR kTimerDestroy(RK_TIMER_HANDLE *const timerHandlePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TIMER_DESTROY,
                                        (ULONG)(UINTPTR)timerHandlePtr,
                                        0UL, 0UL, 0UL));
    }

    if ((timerHandlePtr == NULL) || (*timerHandlePtr == RK_NULL_HANDLE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_TIMERS_MAX > 0U)
    RK_CR_AREA
    RK_CR_ENTER

    VOID *objPtr = NULL;
    err = kDynObjResolveDynamicHandle(RK_DYN_OBJ_TYPE_TIMER,
                                      *timerHandlePtr, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    RK_TIMER *const timerPtr = (RK_TIMER *)objPtr;

    if (kObjHeaderReady(&timerPtr->header, RK_TIMER_KOBJ_ID) != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    err = kTimerCancel(timerPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    err = kDynObjInvalidateHandle(RK_DYN_OBJ_TYPE_TIMER, *timerHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    kTraceRecordObject(timerPtr, RK_TRACE_OP_FREE, RK_ERR_SUCCESS, 0UL);
    kTraceUnregisterObject(timerPtr);
    err = kDynObjReleaseBlock_(&dynTimerPart, timerPtr,
                               sizeof(RK_TIMER));
    if (err == RK_ERR_SUCCESS)
    {
        *timerHandlePtr = RK_NULL_HANDLE;
    }
    RK_CR_EXIT
    return (err);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}
#endif

#if (RK_CONF_MRM == ON)
static RK_ERR kMRMCreateWithAttr_(RK_MRM_HANDLE *const mrmHandlePtr,
                                  RK_MRM_BUF *const mrmPoolPtr,
                                  VOID *mesgPoolPtr,
                                  ULONG const nBufs,
                                  ULONG const dataSizeWords,
                                  RK_OBJ_ATTR const *const attrPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    RK_ERR err = kDynObjScopeAttrErr_(attrPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
    if ((attrPtr != NULL) && (attrPtr->scope != RK_SCOPE_MODULE_LOCAL))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    err = kDynObjCheckCreateHandle_(mrmHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_MRMS_MAX > 0U)
    RK_MRM *const mrmPtr = (RK_MRM *)kMemPartitionAlloc(&dynMrmPart);
    if (mrmPtr == NULL)
    {
        return (RK_ERR_BUFFER_EMPTY);
    }

    RK_MEMSET(mrmPtr, 0, sizeof(RK_MRM));
    kKernelConstructionEnter();
    err = kMRMInit(mrmPtr, mrmPoolPtr, mesgPoolPtr, nBufs, dataSizeWords);
    kKernelConstructionExit();
    if (err != RK_ERR_SUCCESS)
    {
        RK_MEMSET(mrmPtr, 0, sizeof(RK_MRM));
        kMemPartitionFree(&dynMrmPart, mrmPtr);
        return (err);
    }

    err = kDynObjApplyScope_(&mrmPtr->header, attrPtr);
    if (err == RK_ERR_SUCCESS)
    {
        err = kDynObjApplyScope_(&mrmPtr->mrmMem.header, attrPtr);
    }
    if (err == RK_ERR_SUCCESS)
    {
        err = kDynObjApplyScope_(&mrmPtr->mrmDataMem.header, attrPtr);
    }
    if (err != RK_ERR_SUCCESS)
    {
        kTraceUnregisterObject(mrmPtr);
        kTraceUnregisterObject(&mrmPtr->mrmDataMem);
        kTraceUnregisterObject(&mrmPtr->mrmMem);
        RK_MEMSET(mrmPtr, 0, sizeof(RK_MRM));
        kMemPartitionFree(&dynMrmPart, mrmPtr);
        return (err);
    }

    RK_HANDLE handle = RK_NULL_HANDLE;
    err = kDynObjPublishHandle(RK_DYN_OBJ_TYPE_MRM, mrmPtr, &handle);
    if (err != RK_ERR_SUCCESS)
    {
        kTraceUnregisterObject(mrmPtr);
        kTraceUnregisterObject(&mrmPtr->mrmDataMem);
        kTraceUnregisterObject(&mrmPtr->mrmMem);
        RK_MEMSET(mrmPtr, 0, sizeof(RK_MRM));
        kMemPartitionFree(&dynMrmPart, mrmPtr);
        return (err);
    }

    *mrmHandlePtr = (RK_MRM_HANDLE)handle;
    return (RK_ERR_SUCCESS);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}

RK_ERR kMRMCreate(RK_MRM_HANDLE *const mrmHandlePtr,
                  RK_MRM_BUF *const mrmPoolPtr,
                  VOID *mesgPoolPtr,
                  ULONG const nBufs,
                  ULONG const dataSizeWords)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        RK_MRM_CREATE_SYSCALL_ARGS syscallArgs;

        syscallArgs.mrmHandlePtr = mrmHandlePtr;
        syscallArgs.mrmPoolPtr = mrmPoolPtr;
        syscallArgs.mesgPoolPtr = mesgPoolPtr;
        syscallArgs.nBufs = nBufs;
        syscallArgs.dataSizeWords = dataSizeWords;

        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MRM_CREATE,
                                        (ULONG)(UINTPTR)&syscallArgs,
                                        0UL, 0UL, 0UL));
    }

    return (kMRMCreateWithAttr_(mrmHandlePtr, mrmPoolPtr, mesgPoolPtr,
                                nBufs, dataSizeWords, NULL));
}

RK_ERR kMRMCreateModuleScope(RK_MRM_HANDLE *const mrmHandlePtr,
                             RK_MRM_BUF *const mrmPoolPtr,
                             VOID *mesgPoolPtr,
                             ULONG const nBufs,
                             ULONG const dataSizeWords,
                             RK_MODULE *const modulePtr)
{
    RK_OBJ_ATTR const attr = { RK_SCOPE_MODULE_LOCAL, modulePtr };
    return (kMRMCreateWithAttr_(mrmHandlePtr, mrmPoolPtr, mesgPoolPtr,
                                nBufs, dataSizeWords, &attr));
}

RK_ERR kMRMDestroy(RK_MRM_HANDLE *const mrmHandlePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_MRM_DESTROY,
                                        (ULONG)(UINTPTR)mrmHandlePtr,
                                        0UL, 0UL, 0UL));
    }

    if ((mrmHandlePtr == NULL) || (*mrmHandlePtr == RK_NULL_HANDLE))
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
#endif
        return (RK_ERR_OBJ_NULL);
    }

    RK_ERR err = kDynObjCheckReady_();
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

#if (RK_CONF_DYNAMIC_MRMS_MAX > 0U)
    RK_CR_AREA
    RK_CR_ENTER

    VOID *objPtr = NULL;
    err = kDynObjResolveDynamicHandle(RK_DYN_OBJ_TYPE_MRM,
                                      *mrmHandlePtr, &objPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }
    RK_MRM *const mrmPtr = (RK_MRM *)objPtr;

    if (kObjHeaderReady(&mrmPtr->header, RK_MRM_KOBJ_ID) != RK_TRUE)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
#endif
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    RK_MODULE const *const callerModulePtr =
        (RK_gRunPtr != NULL) ? RK_gRunPtr->modulePtr : NULL;
    err = ((callerModulePtr != NULL) && (mrmPtr->ownerModulePtr == NULL))
              ? RK_ERR_INVALID_PARAM
              : kObjHeaderModuleLocalAccessErr(&mrmPtr->header,
                                                callerModulePtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    ULONG const currentHeld = (mrmPtr->currBufPtr != NULL) ? 1UL : 0UL;
    if (((mrmPtr->currBufPtr != NULL) &&
         (mrmPtr->currBufPtr->nUsers > 0UL)) ||
        (kMRMHasActiveLease(mrmPtr) == RK_TRUE) ||
        ((mrmPtr->mrmMem.nMaxBlocks - mrmPtr->mrmMem.nFreeBlocks) !=
         currentHeld) ||
        ((mrmPtr->mrmDataMem.nMaxBlocks - mrmPtr->mrmDataMem.nFreeBlocks) !=
         currentHeld))
    {
        RK_CR_EXIT
        return (kDynObjInvalidState_());
    }

    if (mrmPtr->currBufPtr != NULL)
    {
        VOID *const dataPtr = mrmPtr->currBufPtr->mrmData;
        err = kMemPartitionFree(&mrmPtr->mrmDataMem, dataPtr);
        if (err != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (err);
        }
        err = kMemPartitionFree(&mrmPtr->mrmMem, mrmPtr->currBufPtr);
        if (err != RK_ERR_SUCCESS)
        {
            RK_CR_EXIT
            return (err);
        }
        mrmPtr->currBufPtr = NULL;
    }

    err = kDynObjInvalidateHandle(RK_DYN_OBJ_TYPE_MRM, *mrmHandlePtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (err);
    }

    kTraceUnregisterObject(&mrmPtr->mrmDataMem);
    kTraceUnregisterObject(&mrmPtr->mrmMem);
    kTraceRecordObject(mrmPtr, RK_TRACE_OP_FREE, RK_ERR_SUCCESS, 0UL);
    kTraceUnregisterObject(mrmPtr);
    err = kDynObjReleaseBlock_(&dynMrmPart, mrmPtr,
                               sizeof(RK_MRM));
    if (err == RK_ERR_SUCCESS)
    {
        *mrmHandlePtr = RK_NULL_HANDLE;
    }
    RK_CR_EXIT
    return (err);
#else
    return (RK_ERR_BUFFER_EMPTY);
#endif
}
#endif
