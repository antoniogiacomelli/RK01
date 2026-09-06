/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_SYSCALL_H
#define RK_SYSCALL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <kcommondefs.h>
#include <kcoredefs.h>
#include <kmpu.h>

#define RK_ERR_SYSCALL_RESTART ((RK_ERR)902)

#define RK_SYSCALL_NONE (0UL)
#define RK_SYSCALL_YIELD (1UL)
#define RK_SYSCALL_TASK_GET_RUNNING_HANDLE (2UL)
#define RK_SYSCALL_TASK_GET_ID (3UL)
#define RK_SYSCALL_TASK_GET_RUNNING_NAME (4UL)
#define RK_SYSCALL_TASK_GET_NAME (5UL)
#define RK_SYSCALL_TASK_GET_PRIO (6UL)
#define RK_SYSCALL_SCH_LOCK (7UL)
#define RK_SYSCALL_SCH_UNLOCK (8UL)
#define RK_SYSCALL_TASK_INIT (9UL)
#define RK_SYSCALL_TASK_INIT_MODULE (10UL)
#define RK_SYSCALL_TASK_INIT_PROTECTED (11UL)
#define RK_SYSCALL_TASK_SPAWN (12UL)
#define RK_SYSCALL_TASK_TERMINATE (13UL)
#define RK_SYSCALL_TASK_TERMINATE_SELF (14UL)
#define RK_SYSCALL_MODULE_INIT (15UL)
#define RK_SYSCALL_SHARED_REGION_INIT (16UL)
#define RK_SYSCALL_MODULE_MAP_SHARED_REGION (17UL)
#define RK_SYSCALL_OBJ_PARTITIONS_INIT (18UL)
#define RK_SYSCALL_TASK_GET_NOM_PRIO (19UL)

#define RK_SYSCALL_SHARED_MEM_CREATE (30UL)
#define RK_SYSCALL_SHARED_MEM_DESTROY (31UL)
#define RK_SYSCALL_SHARED_MEM_ATTACH (32UL)
#define RK_SYSCALL_SHARED_MEM_DETACH (33UL)
#define RK_SYSCALL_SHARED_MEM_GET (34UL)

#define RK_SYSCALL_TICK_GET (20UL)
#define RK_SYSCALL_TICK_GET_MS (21UL)
#define RK_SYSCALL_SLEEP_DELAY (22UL)
#define RK_SYSCALL_SLEEP_RELEASE (23UL)
#define RK_SYSCALL_SLEEP_UNTIL (24UL)
#define RK_SYSCALL_DELAY (25UL)

#define RK_SYSCALL_MEM_PARTITION_ALLOC (40UL)
#define RK_SYSCALL_MEM_PARTITION_FREE (41UL)
#define RK_SYSCALL_MEM_PARTITION_INIT (42UL)

#define RK_SYSCALL_EVENT_GET (60UL)
#define RK_SYSCALL_EVENT_SET (61UL)
#define RK_SYSCALL_EVENT_CLEAR (62UL)
#define RK_SYSCALL_EVENT_QUERY (63UL)

#define RK_SYSCALL_SEMAPHORE_PEND (80UL)
#define RK_SYSCALL_SEMAPHORE_POST (81UL)
#define RK_SYSCALL_SEMAPHORE_QUERY (82UL)
#define RK_SYSCALL_SEMAPHORE_CREATE (83UL)
#define RK_SYSCALL_SEMAPHORE_DESTROY (84UL)
#define RK_SYSCALL_SEMAPHORE_INIT (85UL)

#define RK_SYSCALL_MUTEX_LOCK (100UL)
#define RK_SYSCALL_MUTEX_UNLOCK (101UL)
#define RK_SYSCALL_MUTEX_QUERY (102UL)
#define RK_SYSCALL_MUTEX_CREATE (103UL)
#define RK_SYSCALL_MUTEX_DESTROY (104UL)
#define RK_SYSCALL_MUTEX_INIT (105UL)

#define RK_SYSCALL_SLEEP_QUEUE_SLEEP (120UL)
#define RK_SYSCALL_SLEEP_QUEUE_SIGNAL (121UL)
#define RK_SYSCALL_SLEEP_QUEUE_READY (122UL)
#define RK_SYSCALL_SLEEP_QUEUE_UNREADY (123UL)
#define RK_SYSCALL_SLEEP_QUEUE_QUERY (124UL)
#define RK_SYSCALL_SLEEP_QUEUE_WAKE (125UL)
#define RK_SYSCALL_SLEEP_QUEUE_CREATE (126UL)
#define RK_SYSCALL_SLEEP_QUEUE_DESTROY (127UL)
#define RK_SYSCALL_SLEEP_QUEUE_INIT (128UL)

#define RK_SYSCALL_CONDVAR_WAIT (140UL)
#define RK_SYSCALL_CONDVAR_SIGNAL (141UL)
#define RK_SYSCALL_CONDVAR_BROADCAST (142UL)
#define RK_SYSCALL_CONDVAR_INIT (143UL)

#define RK_SYSCALL_MESG_QUEUE_SEND (160UL)
#define RK_SYSCALL_MESG_QUEUE_RECV (161UL)
#define RK_SYSCALL_MESG_QUEUE_PEEK (162UL)
#define RK_SYSCALL_MESG_QUEUE_JAM (163UL)
#define RK_SYSCALL_MESG_QUEUE_QUERY (164UL)
#define RK_SYSCALL_MESG_QUEUE_RESET (165UL)
#define RK_SYSCALL_MESG_QUEUE_POST_OVW (166UL)
#define RK_SYSCALL_MESG_QUEUE_BROADCAST (167UL)
#define RK_SYSCALL_MESG_QUEUE_BROADCAST_RECV (168UL)
#define RK_SYSCALL_MESG_QUEUE_CREATE (169UL)
#define RK_SYSCALL_MESG_QUEUE_DESTROY (170UL)
#define RK_SYSCALL_MESG_QUEUE_INIT (171UL)
#define RK_SYSCALL_MESG_QUEUE_INSTALL_SEND_CBK (172UL)

#define RK_SYSCALL_MESG_ENDPOINT_INIT (180UL)
#define RK_SYSCALL_MESG_POOL_INIT (181UL)
#define RK_SYSCALL_MESG_ALLOC (182UL)
#define RK_SYSCALL_MESG_FREE (183UL)
#define RK_SYSCALL_MESG_PAYLOAD (184UL)
#define RK_SYSCALL_MESG_PAYLOAD_CONST (185UL)
#define RK_SYSCALL_MESG_PAYLOAD_BYTES (186UL)
#define RK_SYSCALL_MESG_GET_SENDER_HANDLE (187UL)
#define RK_SYSCALL_MESG_GET_SENDER_ID (188UL)
#define RK_SYSCALL_MESG_SEND (189UL)
#define RK_SYSCALL_MESG_WAIT (190UL)
#define RK_SYSCALL_MESG_COPY_ENDPOINT_INIT (191UL)
#define RK_SYSCALL_MESG_SEND_COPY (192UL)
#define RK_SYSCALL_MESG_RECV_COPY (193UL)

#define RK_SYSCALL_SYNCH_MESG_INIT (200UL)
#define RK_SYSCALL_SYNCH_SEND_WAIT (201UL)
#define RK_SYSCALL_SYNCH_RECV (202UL)
#define RK_SYSCALL_SYNCH_MESG_CALL (203UL)
#define RK_SYSCALL_SYNCH_MESG_ACCEPT (204UL)
#define RK_SYSCALL_SYNCH_MESG_REPLY (205UL)

#define RK_SYSCALL_MRM_INIT (220UL)
#define RK_SYSCALL_MRM_CREATE (221UL)
#define RK_SYSCALL_MRM_DESTROY (222UL)
#define RK_SYSCALL_MRM_RESERVE (223UL)
#define RK_SYSCALL_MRM_PUBLISH (224UL)
#define RK_SYSCALL_MRM_GET (225UL)
#define RK_SYSCALL_MRM_UNGET (226UL)

#define RK_SYSCALL_TIMER_INIT (240UL)
#define RK_SYSCALL_TIMER_CREATE (241UL)
#define RK_SYSCALL_TIMER_DESTROY (242UL)
#define RK_SYSCALL_TIMER_CANCEL (243UL)
#define RK_SYSCALL_TIMER_RELOAD (244UL)

#define RK_SYSCALL_SYSMON_COMMAND (250UL)

#define RK_SYSCALL_TRACE_INIT (260UL)
#define RK_SYSCALL_TRACE_POLL (261UL)
#define RK_SYSCALL_TRACE_INPUT_SIGNAL (262UL)
#define RK_SYSCALL_TRACE_OBJECT_NAME_SET (263UL)
#define RK_SYSCALL_TRACE_RECORD_OBJECT (264UL)
#define RK_SYSCALL_TRACE_RECORD_TASK_PRIO (265UL)
#define RK_SYSCALL_TRACE_RECORD_TASK_OVERRUN (266UL)
#define RK_SYSCALL_TRACE_TASK_SNAPSHOT (267UL)
#define RK_SYSCALL_TRACE_MESG_SNAPSHOT (268UL)
#define RK_SYSCALL_TRACE_SEMA_SNAPSHOT (269UL)
#define RK_SYSCALL_TRACE_TIMER_SNAPSHOT (270UL)
#define RK_SYSCALL_TRACE_RECORD_SNAPSHOT (271UL)
#define RK_SYSCALL_TRACE_TASK_PRIO_SNAPSHOT (272UL)
#define RK_SYSCALL_TRACE_REGISTER_OBJECT (273UL)
#define RK_SYSCALL_TRACE_UNREGISTER_OBJECT (274UL)
#define RK_SYSCALL_TRACE_OVERFLOW_PERSIST (275UL)

#define RK_SYSCALL_GET_VERSION (280UL)
#define RK_SYSCALL_ERR_HANDLER (281UL)

#define RK_SYSCALL_LOG_WRITE (300UL)
#define RK_SYSCALL_LOG_CONSUME (301UL)
#define RK_SYSCALL_LOG_FORMAT (302UL)

#define RK_SYSCALL_CONSOLE_WRITE (320UL)

#define RK_SYSCALL_APP_BASE (0x8000UL)
#define RK_SYSCALL_TEST_SYSTICK_DEFER (RK_SYSCALL_APP_BASE + 0x100UL)

#define RK_SYSCALL_PHASE_NONE (0U)
#define RK_SYSCALL_PHASE_WAIT (1U)
#define RK_SYSCALL_PHASE_CONDVAR_RELOCK (2U)

extern volatile unsigned RK_gSyscallThreadModeActive;

typedef struct RK_STRUCT_TASK_INIT_SYSCALL_ARGS
{
    RK_TASK_HANDLE *taskHandlePtr;
    RK_TASKENTRY taskFunc;
    VOID *argsPtr;
    CHAR *taskName;
    RK_STACK *stackBufPtr;
    ULONG stackSize;
    RK_PRIO priority;
    RK_OPTION preempt;
    RK_MODULE *modulePtr;
} RK_TASK_INIT_SYSCALL_ARGS;

typedef struct RK_STRUCT_TASK_PROTECTED_INIT_SYSCALL_ARGS
{
    RK_TCB *taskPtr;
    RK_TASKENTRY taskFunc;
    VOID *argsPtr;
    RK_PRIO priority;
    RK_TASK_MEMORY const *memoryPtr;
} RK_TASK_PROTECTED_INIT_SYSCALL_ARGS;

#if (RK_CONF_CALLOUT_TIMER == ON)
typedef struct RK_STRUCT_TIMER_INIT_SYSCALL_ARGS
{
    RK_TIMER *timerPtr;
    RK_TICK phase;
    RK_TICK countTicks;
    RK_TIMER_CALLOUT funPtr;
    VOID *argsPtr;
    RK_OPTION reload;
} RK_TIMER_INIT_SYSCALL_ARGS;

typedef struct RK_STRUCT_TIMER_CREATE_SYSCALL_ARGS
{
    RK_TIMER_HANDLE *timerHandlePtr;
    CHAR *objName;
    RK_TICK phase;
    RK_TICK countTicks;
    RK_TIMER_CALLOUT funPtr;
    VOID *argsPtr;
    RK_OPTION reload;
} RK_TIMER_CREATE_SYSCALL_ARGS;
#endif

#if (RK_CONF_MESG_QUEUE == ON)
typedef struct RK_STRUCT_MESG_QUEUE_CREATE_SYSCALL_ARGS
{
    RK_MESG_QUEUE_HANDLE *queueHandlePtr;
    CHAR *objName;
    VOID *bufPtr;
    ULONG mesgWords;
    ULONG depth;
} RK_MESG_QUEUE_CREATE_SYSCALL_ARGS;
#endif

#if (RK_CONF_MRM == ON)
typedef struct RK_STRUCT_MRM_INIT_SYSCALL_ARGS
{
    RK_MRM *mrmPtr;
    RK_MRM_BUF *mrmPoolPtr;
    VOID *mesgPoolPtr;
    ULONG nBufs;
    ULONG dataSizeWords;
} RK_MRM_INIT_SYSCALL_ARGS;

typedef struct RK_STRUCT_MRM_CREATE_SYSCALL_ARGS
{
    RK_MRM_HANDLE *mrmHandlePtr;
    CHAR *objName;
    RK_MRM_BUF *mrmPoolPtr;
    VOID *mesgPoolPtr;
    ULONG nBufs;
    ULONG dataSizeWords;
} RK_MRM_CREATE_SYSCALL_ARGS;
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
typedef struct RK_STRUCT_MESG_POOL_INIT_SYSCALL_ARGS
{
    RK_MEM_PARTITION *poolPtr;
    VOID *memPoolPtr;
    ULONG payloadBytes;
    ULONG nMesg;
    RK_PRIO ceilingPrio;
} RK_MESG_POOL_INIT_SYSCALL_ARGS;

#if (RK_CONF_ASYNCH_COPY_MESG == ON)
typedef struct RK_STRUCT_MESG_RECV_COPY_SYSCALL_ARGS
{
    RK_TASK_HANDLE fromTaskHandle;
    VOID *recvPtr;
    ULONG recvBytes;
    ULONG *rxBytesPtr;
    RK_TICK timeout;
} RK_MESG_RECV_COPY_SYSCALL_ARGS;
#endif
#endif

RK_BOOL kSyscallRequired(VOID);
VOID kSyscallTaskSuspend(RK_EXCEPTION_FRAME *const framePtr,
                         ULONG const callNumber,
                         ULONG const arg0,
                         ULONG const arg1,
                         ULONG const arg2,
                         ULONG const arg3,
                         UINT const phase);
VOID kSyscallTaskCheckpoint(RK_EXCEPTION_FRAME *const framePtr,
                            ULONG const callNumber,
                            ULONG const arg0,
                            ULONG const arg1,
                            ULONG const arg2,
                            ULONG const arg3);
VOID kSyscallTaskClear(RK_TCB *const taskPtr);
VOID kSyscallTaskWake(RK_TCB *const taskPtr);
VOID kSyscallTaskTimeout(RK_TCB *const taskPtr);
RK_BOOL kSyscallDispatchApp(RK_EXCEPTION_FRAME *const framePtr,
                            ULONG const callNumber,
                            ULONG const arg0,
                            ULONG const arg1,
                            ULONG const arg2,
                            ULONG const arg3);
RK_BOOL kLoggerSyscallDispatch(RK_EXCEPTION_FRAME *const framePtr,
                               ULONG const callNumber,
                               ULONG const arg0,
                               ULONG const arg1,
                               ULONG const arg2,
                               ULONG const arg3);

RK_FORCE_INLINE
static inline ULONG kSyscallInvoke4(ULONG const callNumber,
                                    ULONG const arg0,
                                    ULONG const arg1,
                                    ULONG const arg2,
                                    ULONG const arg3)
{
    ULONG ret;

    do
    {
        ret = kSyscall4_(callNumber, arg0, arg1, arg2, arg3);
    } while ((RK_ERR)ret == RK_ERR_SYSCALL_RESTART);

    return (ret);
}

RK_ERR kSleepDelaySyscall(RK_EXCEPTION_FRAME *const framePtr,
                          RK_TICK const ticks);
RK_ERR kSleepReleaseSyscall(RK_EXCEPTION_FRAME *const framePtr,
                            RK_TICK const period);
RK_ERR kSleepUntilSyscall(RK_EXCEPTION_FRAME *const framePtr,
                          RK_TICK *const lastTickPtr,
                          RK_TICK const ticks);

RK_ERR kEventGetSyscall(RK_EXCEPTION_FRAME *const framePtr,
                        RK_TASK_EVENT const requiredFlags,
                        RK_OPTION const getOptions,
                        RK_TASK_EVENT *const gotFlagsPtr,
                        RK_TICK const timeout);

#if (RK_CONF_SEMAPHORE == ON)
RK_ERR kSemaphorePendSyscall(RK_EXCEPTION_FRAME *const framePtr,
                             RK_SEMAPHORE_HANDLE const semaHandle,
                             RK_TICK const timeout);
#endif

#if (RK_CONF_MUTEX == ON)
RK_ERR kMutexLockSyscall(RK_EXCEPTION_FRAME *const framePtr,
                         RK_MUTEX_HANDLE const mutexHandle,
                         RK_TICK const timeout);
RK_ERR kMutexLockSyscallContinue(RK_EXCEPTION_FRAME *const framePtr,
                                 RK_MUTEX_HANDLE const mutexHandle,
                                 RK_TICK const timeout,
                                 ULONG const callNumber,
                                 ULONG const arg0,
                                 ULONG const arg1,
                                 ULONG const arg2,
                                 ULONG const arg3,
                                 UINT const phase);
#endif

#if (RK_CONF_SLEEP_QUEUE == ON)
RK_ERR kSleepQueueSleepSyscall(RK_EXCEPTION_FRAME *const framePtr,
                               RK_SLEEP_QUEUE_HANDLE const sleepqHandle,
                               RK_TICK const timeout);
#if (RK_CONF_CONDVAR == ON)
RK_ERR kCondVarWaitSyscall(RK_EXCEPTION_FRAME *const framePtr,
                           RK_SLEEP_QUEUE_HANDLE const condHandle,
                           RK_MUTEX_HANDLE const lockHandle,
                           RK_TICK const timeout);
#endif
#endif

#if (RK_CONF_MESG_QUEUE == ON)
RK_ERR kMesgQueueSendSyscall(RK_EXCEPTION_FRAME *const framePtr,
                             RK_MESG_QUEUE_HANDLE const queueHandle,
                             VOID *const sendPtr,
                             RK_TICK const timeout);
RK_ERR kMesgQueueRecvSyscall(RK_EXCEPTION_FRAME *const framePtr,
                             RK_MESG_QUEUE_HANDLE const queueHandle,
                             VOID *const recvPtr,
                             RK_TICK const timeout);
RK_ERR kMesgQueueJamSyscall(RK_EXCEPTION_FRAME *const framePtr,
                            RK_MESG_QUEUE_HANDLE const queueHandle,
                            VOID *const sendPtr,
                            RK_TICK const timeout);
RK_ERR kMesgQueueBroadcastRecvSyscall(RK_EXCEPTION_FRAME *const framePtr,
                                      RK_MESG_QUEUE_HANDLE const queueHandle,
                                      VOID *const recvPtr,
                                      RK_TICK const timeout);
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
RK_ERR kMesgAllocSyscall(RK_EXCEPTION_FRAME *const framePtr,
                         RK_MEM_PARTITION *const poolPtr,
                         RK_MESG **const mesgPPtr,
                         RK_TICK const timeout);
RK_ERR kMesgWaitSyscall(RK_EXCEPTION_FRAME *const framePtr,
                        RK_TASK_HANDLE const fromTaskHandle,
                        RK_MESG **const mesgPPtr,
                        RK_TICK const timeout);
#if (RK_CONF_ASYNCH_COPY_MESG == ON)
RK_ERR kMesgRecvCopySyscall(
    RK_EXCEPTION_FRAME *const framePtr,
    ULONG const userArgsAddr,
    RK_MESG_RECV_COPY_SYSCALL_ARGS const *const argsPtr);
#endif
#endif

#if (RK_CONF_SYNCH_MESG == ON)
RK_ERR kSynchSendWaitSyscall(RK_EXCEPTION_FRAME *const framePtr,
                             RK_TASK_HANDLE const taskHandle,
                             VOID const *const mesgPtr,
                             ULONG const mesgBytes,
                             RK_TICK const timeout);
RK_ERR kSyncRecvSyscall(RK_EXCEPTION_FRAME *const framePtr,
                        VOID *const recvPtr,
                        ULONG *const mesgBytesPtr,
                        RK_TICK const timeout);
RK_ERR kSynchMesgCallSyscall(RK_EXCEPTION_FRAME *const framePtr,
                             RK_TASK_HANDLE const taskHandle,
                             RK_SYNCH_ATTR const *const attrPtr,
                             RK_TICK const timeout);
RK_ERR kSynchMesgAcceptSyscall(RK_EXCEPTION_FRAME *const framePtr,
                               RK_SYNCH_CALL_DATA *const callPtr,
                               VOID *const recvPtr,
                               ULONG *const reqBytesPtr,
                               RK_TICK const timeout);
RK_ERR kConsoleWriteSyscall(RK_EXCEPTION_FRAME *const framePtr,
                            RK_SYNCH_ATTR const *const attrPtr,
                            RK_TICK const timeout);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RK_SYSCALL_H */
