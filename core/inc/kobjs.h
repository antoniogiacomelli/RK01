/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_OBJS_H
#define RK_OBJS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <kmpu.h>

struct RK_STRUCT_RING_BUFFER
{
    ULONG dataSize;
    ULONG maxBuf;
    ULONG nFull;
    ULONG *bufPtr;
    ULONG *writePtr;
    ULONG *readPtr;
    ULONG *bufEndPtr;
} K_ALIGN(4);
struct  RK_STRUCT_TIMEOUT_NODE
{
    struct RK_STRUCT_TIMEOUT_NODE *nextPtr;
    struct RK_STRUCT_TIMEOUT_NODE *prevPtr;
    volatile struct RK_STRUCT_TIMEOUT_NODE **listRefPtr;
    UINT timeoutType;
    RK_TICK timeout;
    RK_TICK dtick;
    RK_LIST *waitingQueuePtr;
    UINT waitInfo;    /* object-specific wake context */
} K_ALIGN(4);

struct RK_STRUCT_LIST_NODE
{
    struct RK_STRUCT_LIST_NODE *nextPtr;
    struct RK_STRUCT_LIST_NODE *prevPtr;
} K_ALIGN(4);

struct RK_STRUCT_LIST
{
    struct RK_STRUCT_LIST_NODE listDummy;
    ULONG size;
} K_ALIGN(4);

struct RK_OBJ_TCB;

/*
 * Common header for initialised, named kernel objects.
 *
 * Keep objID first: the dynamic-object registry and trace code both rely on
 * reading the first word of an object as its type tag before doing any
 * object-specific cast.
 */
struct RK_STRUCT_KOBJ
{
    RK_ID objID;
    CHAR objName[RK_NAME_SIZE];
    RK_BOOL init;
    RK_OBJ_SCOPE scope;
    RK_MODULE *ownerModulePtr;
#if (RK_CONF_SYSMON == ON)
    struct RK_STRUCT_LIST_NODE sysMonNode;
    RK_BOOL sysMonListed;
#endif
};

#if (RK_CONF_SYSMON == ON)
#define RK_OBJ_HEADER_SYSMON_FIELDS                                           \
            struct RK_STRUCT_LIST_NODE sysMonNode;                            \
            RK_BOOL sysMonListed;
#else
#define RK_OBJ_HEADER_SYSMON_FIELDS
#endif

#define RK_OBJ_HEADER                                                         \
    __extension__ union                                                       \
    {                                                                         \
        RK_OBJ header;                                                        \
        struct                                                                \
        {                                                                     \
            RK_ID objID;                                                      \
            CHAR objName[RK_NAME_SIZE];                                       \
            RK_BOOL init;                                                     \
            RK_OBJ_SCOPE scope;                                               \
            RK_MODULE *ownerModulePtr;                                        \
            RK_OBJ_HEADER_SYSMON_FIELDS                                       \
        };                                                                    \
    }

#define RK_KOBJ_HEADER RK_OBJ_HEADER

static inline RK_BOOL kObjHeaderReady(RK_KOBJ const *const headerPtr,
                                      RK_ID const objID)
{
    return (((headerPtr != NULL) &&
             (headerPtr->objID == objID) &&
             (headerPtr->init == RK_TRUE))
                ? RK_TRUE
                : RK_FALSE);
}

static inline RK_ERR kObjHeaderReadyErr(RK_KOBJ const *const headerPtr,
                                        RK_ID const objID)
{
    if (headerPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (headerPtr->objID != objID)
    {
        return (RK_ERR_INVALID_OBJ);
    }

    if (headerPtr->init != RK_TRUE)
    {
        return (RK_ERR_OBJ_NOT_INIT);
    }

    return (RK_ERR_SUCCESS);
}

static inline VOID kObjHeaderOwnerModuleSet(RK_KOBJ *const headerPtr,
                                            RK_MODULE *const modulePtr)
{
    if (headerPtr != NULL)
    {
        headerPtr->scope = (modulePtr != NULL) ? RK_SCOPE_MODULE_LOCAL
                                               : RK_SCOPE_UNASSIGNED;
        headerPtr->ownerModulePtr = modulePtr;
    }
}

static inline RK_ERR kObjHeaderScopeSet(RK_KOBJ *const headerPtr,
                                        RK_OBJ_SCOPE const scope,
                                        RK_MODULE *const modulePtr)
{
    if (headerPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (((scope == RK_SCOPE_MODULE_LOCAL) && (modulePtr == NULL)) ||
        ((scope == RK_SCOPE_KERNEL_GLOBAL) && (modulePtr != NULL)) ||
        (scope == RK_SCOPE_UNASSIGNED))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    headerPtr->scope = scope;
    headerPtr->ownerModulePtr =
        (scope == RK_SCOPE_MODULE_LOCAL) ? modulePtr : NULL;
    return (RK_ERR_SUCCESS);
}

static inline RK_ERR
kObjHeaderModuleLocalAccessErr(RK_KOBJ const *const headerPtr,
                               RK_MODULE const *const callerModulePtr)
{
    if (headerPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    if (headerPtr->scope == RK_SCOPE_KERNEL_GLOBAL)
    {
        return (RK_ERR_SUCCESS);
    }

    if (headerPtr->scope == RK_SCOPE_UNASSIGNED)
    {
        return ((callerModulePtr == NULL) ? RK_ERR_SUCCESS
                                          : RK_ERR_INVALID_PARAM);
    }

    if ((headerPtr->scope != RK_SCOPE_MODULE_LOCAL) ||
        (headerPtr->ownerModulePtr == NULL))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    if ((callerModulePtr != NULL) &&
        (headerPtr->ownerModulePtr != callerModulePtr))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    return (RK_ERR_SUCCESS);
}

struct RK_OBJ_SHARED_REGION
{
    BYTE *regionBasePtr;
    ULONG regionBytes;
    RK_BOOL init;
} K_ALIGN(4);

struct RK_OBJ_SHARED_MEM
{
    RK_KOBJ_HEADER;
    RK_SHARED_REGION region;
    RK_MODULE *attachedModulePtr[RK_NTHREADS];
    ULONG attachCount;
} K_ALIGN(4);

struct RK_OBJ_MODULE
{
    CHAR moduleName[RK_NAME_SIZE];
    BYTE *regionBasePtr;
    ULONG regionBytes;
    ULONG allocBytes;
    RK_SHARED_REGION *sharedRegionPtr[RK_CONF_MODULE_SHARED_REGIONS];
    ULONG taskCount;
    RK_BOOL init;
} K_ALIGN(4);

#if (RK_CONF_DYNAMIC_TASK == ON)
struct RK_STRUCT_DYNAMIC_TASK_ATTR
{
    RK_TASKENTRY taskFunc;
    VOID *argsPtr;
    CHAR *taskName;
    RK_PRIO priority;
    RK_OPTION preempt;
    RK_MEM_PARTITION *stackMemPtr;
    RK_MODULE *modulePtr;
} K_ALIGN(4);
#endif

struct  RK_OBJ_TCB
{
    /* Context-switch assembly depends on this leading layout. */
    /* --- dont change begin --- */
    UINT *sp;
    RK_TASK_STATUS status;
    ULONG runCnt;
    UINT savedLR;
    RK_STACK *stackBufPtr;
    CHAR taskName[RK_OBJ_MAX_NAME_LEN];
    ULONG stackSize;
    RK_TID tid; /* System-defined task ID */

    /*priority range: 0...31, highest to lowest */
    RK_PRIO priority;    /* Effective priority (in-use) */
    RK_PRIO prioNominal; /* Nominal assigned  priority  */
    ULONG preempt;       /* 1 if task is preemptable, 0 if not (exceptional) */
    ULONG schLock;       /* Scheduler lock depth owned */
    RK_BOOL init;
    /* --- dont change end --- */

    ULONG savedControl;
    BYTE *taskMemoryBasePtr;
    ULONG taskMemoryBytes;
    RK_MODULE *modulePtr;
    RK_MODULE privateModule;
    RK_MPU_REGION mpuRegion[RK_MPU_N_REGIONS];
    RK_EXCEPTION_FRAME *syscallFramePtr;
    ULONG syscallNumber;
    ULONG syscallArg0;
    ULONG syscallArg1;
    ULONG syscallArg2;
    ULONG syscallArg3;
    UINT syscallPhase;
    RK_ERR syscallWakeResult;

    /* sleep-timers */
    /* on every sleep-release-until call this
    field is computed and replaced */
    RK_TICK wakeTime;
    /*
    overrun count for sleep-release/until
    */
    ULONG overrunCount;
    /*
    this flag is only true when a bounded waiting expires
    not for sleep timers
    */
    RK_BOOL timeOut;

    /* Event Flags */
    RK_TASK_EVENT flagsCurr; /* events signalled to this task */
    RK_OPTION flagsOpt;  /* a task expects ANY or ALL of */
    RK_TASK_EVENT flagsReq;  /* the events set here */


#if (RK_CONF_MESG_QUEUE == ON)
    VOID *mesgQueueRecvBufPtr;
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
    RK_BOOL asynchMesgInit;
    struct RK_STRUCT_LIST asynchMesgQueue;
    struct RK_STRUCT_LIST asynchMesgWaiters;
    /* Messages owned by this task; scheduler scans it for pool ceilings. */
    struct RK_STRUCT_LIST asynchMesgOwnedList;
    struct RK_OBJ_TCB *asynchMesgWaitSenderPtr;
    RK_MESG **asynchMesgWaitDestPtr;
    RK_MESG **asynchMesgAllocDestPtr;
    RK_ERR asynchMesgWaitStatus;
#if (RK_CONF_ASYNCH_COPY_MESG == ON)
    RK_BOOL asynchCopyMesgInit;
    struct RK_STRUCT_LIST asynchCopyMesgQueue;
    struct RK_STRUCT_LIST asynchCopyMesgWaiters;
    struct RK_OBJ_TCB *asynchCopyMesgWaitSenderPtr;
    VOID *asynchCopyMesgRecvBufPtr;
    ULONG asynchCopyMesgRecvBufBytes;
    ULONG *asynchCopyMesgRecvBytesPtr;
    RK_ERR asynchCopyMesgRecvStatus;
#endif
#endif /* RK_CONF_ASYNCH_MESG && RK_CONF_MESG_QUEUE */

#if (RK_CONF_SYNCH_MESG == ON)
    ULONG synchMesgMaxBytes;
    VOID const *synchMesgPendingPtr;
    struct RK_OBJ_TCB *synchMesgPendingSenderPtr;
    VOID *synchMesgRecvBufPtr;
    ULONG *synchMesgRecvBytesPtr;
    RK_ERR synchMesgRecvStatus;
    struct RK_STRUCT_LIST synchMesgSenders;
    VOID const *synchMesgPtr;
    ULONG synchMesgBytes;
    RK_ERR synchMesgStatus;
    struct RK_OBJ_TCB *synchMesgReceiverPtr;
    struct RK_STRUCT_LIST synchMesgCallers;
    struct RK_STRUCT_LIST synchMesgAcceptWaiters;
    struct RK_OBJ_TCB *synchMesgActiveCallerPtr;
    RK_PRIO synchMesgActiveCallerPrio;
    VOID *synchMesgCallReplyBufPtr;
    ULONG *synchMesgCallReplyBytesPtr;
    ULONG synchMesgCallReplyMaxBytes;
    RK_SYNCH_CALL_STATE synchMesgCallState;
#endif


#if (RK_CONF_MUTEX == ON)
    struct RK_OBJ_MUTEX *waitingForMutexPtr;
    struct RK_STRUCT_LIST ownedMutexList;
#endif

    struct RK_STRUCT_TIMEOUT_NODE timeoutNode;
    struct RK_STRUCT_LIST_NODE tcbNode;

} K_ALIGN(4);

struct RK_STRUCT_RUNTIME
{
    volatile RK_TICK globalTick;
    volatile UINT nWraps;
} K_ALIGN(4);

struct RK_OBJ_MEM_PARTITION
{
    RK_OBJ_HEADER;
    BYTE *freeListPtr;
    BYTE *poolPtr;
    ULONG blkSize;
    ULONG nMaxBlocks;
    ULONG nFreeBlocks;
    struct RK_STRUCT_LIST waitingQueue;
#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
    /* Optional ceiling applied to tasks owning messages from this pool. */
    RK_PRIO mesgPrioCeiling;
    RK_BOOL mesgPrioCeilingEnabled;
#endif
} K_ALIGN(4);

#if (RK_CONF_CALLOUT_TIMER == ON)
struct RK_OBJ_TIMER
{
    RK_OBJ_HEADER;
    UINT reload;
    RK_TICK phase;
    RK_TICK period;
    RK_TICK nextTime;
    RK_TIMER_CALLOUT funPtr;
    VOID *argsPtr;
    struct RK_STRUCT_TIMEOUT_NODE timeoutNode;
} K_ALIGN(4);
#endif

#if (RK_CONF_SEMAPHORE == ON)

struct RK_OBJ_SEMAPHORE
{
    RK_OBJ_HEADER;
    UINT value;
    UINT maxValue;
    struct RK_STRUCT_LIST waitingQueue;
} K_ALIGN(4);

#endif

#if (RK_CONF_MUTEX == ON)

struct RK_OBJ_MUTEX
{
    RK_OBJ_HEADER;
    UINT lock;
    UINT protocol;
    RK_BOOL ownerFaulted;
    struct RK_STRUCT_LIST waitingQueue;
    struct RK_OBJ_TCB *ownerPtr;
    struct RK_STRUCT_LIST_NODE mutexNode;
} K_ALIGN(4);
#endif

#if (RK_CONF_SLEEP_QUEUE == ON)

struct RK_OBJ_SLEEP_QUEUE
{
    RK_OBJ_HEADER;
    struct RK_STRUCT_LIST waitingQueue;
} K_ALIGN(4);

#endif /* RK_CONF_SLEEP_QUEUE */

#if (RK_CONF_MESG_QUEUE == ON)
struct RK_OBJ_MESG_QUEUE
{
    RK_OBJ_HEADER;
    struct RK_STRUCT_LIST waitingReceivers;
    struct RK_STRUCT_LIST waitingSenders;
    struct RK_STRUCT_RING_BUFFER ringBuf;
    ULONG broadcastReceivers;
#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)
    VOID (*sendNotifyCbk)(struct RK_OBJ_MESG_QUEUE *const);
#endif
} K_ALIGN(4);
#endif /* RK_CONF_MESG_QUEUE */

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
struct RK_OBJ_MESG
{
    struct RK_STRUCT_LIST_NODE mesgNode;
    struct RK_STRUCT_LIST_NODE ownerNode;
    RK_MEM_PARTITION *poolPtr;
    RK_TASK_HANDLE sender;
    RK_TASK_HANDLE receiver;
    struct RK_OBJ_TCB *ownerPtr;
    ULONG payloadBytes;
    RK_TID senderPid;
    RK_TID receiverPid;
    RK_MESG_STATE state;
    RK_ID objID;
} K_ALIGN(4);
#endif /* RK_CONF_ASYNCH_MESG && RK_CONF_MESG_QUEUE */

#if (RK_CONF_SYNCH_MESG == ON)
struct RK_STRUCT_SYNCH_ATTR
{
    VOID const *reqPtr;
    ULONG reqBytes;
    VOID *replyPtr;
    ULONG replyMaxBytes;
    ULONG *replyBytesPtr;
} K_ALIGN(4);

struct RK_STRUCT_SYNCH_CALL_DATA
{
    RK_TASK_HANDLE caller;
    VOID *reqPtr;
    VOID *replyPtr;
    ULONG reqBytes;
    ULONG replyMaxBytes;
} K_ALIGN(4);
#endif /* RK_CONF_SYNCH_MESG */

#if (RK_CONF_MRM == ON)

struct RK_OBJ_MRM_BUF
{
    RK_ID objID;
    CHAR objName[RK_NAME_SIZE];
    VOID *mrmData;
    ULONG nUsers; /* number of tasks using */
} K_ALIGN(4);

struct RK_OBJ_MRM
{
    RK_OBJ_HEADER;
    struct RK_OBJ_MEM_PARTITION mrmMem; /* associated allocator */
    struct RK_OBJ_MEM_PARTITION mrmDataMem;
    struct RK_OBJ_MRM_BUF *currBufPtr; /* current buffer   */
    ULONG size;
} K_ALIGN(4);

#endif /* RK_CONF_MRM */

#ifdef __cplusplus
}
#endif

#endif /* RK_OBJS_H */
