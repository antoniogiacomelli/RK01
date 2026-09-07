/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_API_APP_H
#define RK_API_APP_H

#include <kcommondefs.h>
#include <kobjs.h>
#include <klogger.h>

#ifdef __cplusplus
extern "C" {
#endif

void kCoreInit(void);
VOID kInit(VOID);
VOID kErrHandler(RK_FAULT);

RK_ERR kTaskInit(RK_TASK_HANDLE *taskHandlePtr,
                 const RK_TASKENTRY taskFunc,
                 VOID *argsPtr,
                 RK_STRING taskName,
                 RK_STACK *const stackBufPtr,
                 const ULONG stackSize,
                 const RK_PRIO priority,
                 const RK_OPTION preempt);
VOID kYield(VOID);
RK_TASK_HANDLE kTaskGetRunningHandle(VOID);
const CHAR *kTaskGetRunningName(VOID);
RK_TID kTaskGetID(RK_TASK_HANDLE taskHandle);
RK_ERR kTaskGetName(RK_TASK_HANDLE taskHandle, CHAR *buf);
RK_PRIO kTaskGetPrio(RK_TASK_HANDLE taskHandle);
RK_PRIO kTaskGetNomPrio(RK_TASK_HANDLE taskHandle);
VOID kSchLock(VOID);
VOID kSchUnlock(VOID);

RK_ERR kEventGet(RK_TASK_EVENT const required,
                 RK_OPTION const options,
                 RK_TASK_EVENT *const gotFlagsPtr,
                 RK_TICK timeout);
RK_ERR kEventSet(RK_TASK_HANDLE const taskHandle, RK_TASK_EVENT const mask);
RK_ERR kEventQuery(RK_TASK_HANDLE const taskHandle,
                   RK_TASK_EVENT *const gotFlagsPtr);
RK_ERR kEventClear(RK_TASK_HANDLE const taskHandle,
                   RK_TASK_EVENT const flagsToClear);

#if (RK_CONF_MUTEX == ON)
RK_ERR kMutexCreate(RK_MUTEX_HANDLE *const mutexHandlePtr,
                    RK_STRING objName,
                    UINT protocol);
RK_ERR kMutexDestroy(RK_MUTEX_HANDLE *const mutexHandlePtr);
RK_ERR kMutexLock(RK_MUTEX_HANDLE const mutexHandle, RK_TICK const timeout);
RK_ERR kMutexUnlock(RK_MUTEX_HANDLE const mutexHandle);
RK_ERR kMutexQuery(RK_MUTEX_HANDLE const mutexHandle, UINT *const protocolPtr);
#endif

#if (RK_CONF_SEMAPHORE == ON)
RK_ERR kSemaphoreCreate(RK_SEMAPHORE_HANDLE *const semaphoreHandlePtr,
                        RK_STRING objName,
                        UINT const initialValue,
                        UINT const maxValue);
RK_ERR kSemaphoreDestroy(RK_SEMAPHORE_HANDLE *const semaphoreHandlePtr);
RK_ERR kSemaphorePend(RK_SEMAPHORE_HANDLE const semaphoreHandle,
                      RK_TICK const timeout);
RK_ERR kSemaphorePost(RK_SEMAPHORE_HANDLE const semaphoreHandle);
RK_ERR kSemaphoreQuery(RK_SEMAPHORE_HANDLE const semaphoreHandle,
                       INT *const valuePtr);
#endif

#if (RK_CONF_SLEEP_QUEUE == ON)
RK_ERR kSleepQueueCreate(RK_HANDLE *const sleepqHandlePtr,
                         RK_STRING objName);
RK_ERR kSleepQueueDestroy(RK_HANDLE *const sleepqHandlePtr);
RK_ERR kSleepQueueSleep(RK_HANDLE const sleepqHandle,
                        const RK_TICK timeout);
RK_ERR kSleepQueueWake(RK_HANDLE const sleepqHandle,
                       UINT nTasks,
                       UINT *uTasksPtr);
RK_ERR kSleepQueueSignal(RK_HANDLE const sleepqHandle);
RK_ERR kSleepQueueReady(RK_HANDLE const sleepqHandle,
                        RK_TASK_HANDLE taskHandle);
RK_ERR kSleepQueueUnready(RK_HANDLE const sleepqHandle,
                          RK_TASK_HANDLE handle);
RK_ERR kSleepQueueQuery(RK_HANDLE const sleepqHandle,
                        ULONG *const nTasksPtr);
#endif

#if ((RK_CONF_SLEEP_QUEUE == ON) && (RK_CONF_MUTEX == ON) &&                  \
     (RK_CONF_CONDVAR == ON))
RK_ERR kCondVarWait(RK_HANDLE const cv,
                    RK_HANDLE const mutex,
                    RK_TICK timeout);
RK_ERR kCondVarSignal(RK_HANDLE const cv);
RK_ERR kCondVarBroadcast(RK_HANDLE const cv);
#endif

#if (RK_CONF_MESG_QUEUE == ON)
RK_ERR kMesgQueueCreate(RK_MESG_QUEUE_HANDLE *const queueHandlePtr,
                        RK_STRING objName,
                        VOID *const queueBufPtr,
                        ULONG const mesgSizeWords,
                        ULONG const depth);
RK_ERR kMesgQueueDestroy(RK_MESG_QUEUE_HANDLE *const queueHandlePtr);
RK_ERR kMesgQueueSend(RK_MESG_QUEUE_HANDLE const queueHandle,
                      VOID *const sendPtr,
                      const RK_TICK timeout);
RK_ERR kMesgQueueRecv(RK_MESG_QUEUE_HANDLE const queueHandle,
                      VOID *const recvPtr,
                      const RK_TICK timeout);
RK_ERR kMesgQueuePeek(RK_MESG_QUEUE_HANDLE const queueHandle,
                      VOID *const recvPtr);
RK_ERR kMesgQueueJam(RK_MESG_QUEUE_HANDLE const queueHandle,
                     VOID *const sendPtr,
                     const RK_TICK timeout);
RK_ERR kMesgQueueReset(RK_MESG_QUEUE_HANDLE const queueHandle);
RK_ERR kMesgQueueQuery(RK_MESG_QUEUE_HANDLE const queueHandle,
                       UINT *const nMesgPtr,
                       UINT *const nWaitRPtr,
                       UINT *const nWaitSPtr);
RK_ERR kMesgQueuePostOvw(RK_MESG_QUEUE_HANDLE const queueHandle,
                         VOID *sendPtr);
RK_ERR kMesgQueueBroadcast(RK_MESG_QUEUE_HANDLE const queueHandle,
                           VOID *const sendPtr,
                           UINT *const nRecvPtr);
RK_ERR kMesgQueueBroadcastRecv(RK_MESG_QUEUE_HANDLE const queueHandle,
                               VOID *const recvPtr,
                               const RK_TICK timeout);
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
RK_ERR kMesgEndpointInit(RK_TASK_HANDLE const taskHandle);
RK_ERR kMesgPoolInit(RK_MEM_PARTITION *const poolPtr,
                     VOID *const memPoolPtr,
                     ULONG const payloadBytes,
                     ULONG const nMesg,
                     RK_PRIO const ceilingPrio);
RK_ERR kMesgAlloc(RK_MEM_PARTITION *const poolPtr,
                  RK_MESG **const mesgPPtr,
                  RK_TICK const timeout);
RK_ERR kMesgFree(RK_MESG *const mesgPtr);
VOID *kMesgPayload(RK_MESG *const mesgPtr);
VOID const *kMesgPayloadConst(RK_MESG const *const mesgPtr);
ULONG kMesgPayloadBytes(RK_MESG const *const mesgPtr);
RK_TASK_HANDLE kMesgGetSenderHandle(RK_MESG const *const mesgPtr);
RK_ERR kMesgGetSenderID(RK_MESG const *const mesgPtr,
                        RK_TID *const senderIDPtr);
RK_ERR kMesgSend(RK_TASK_HANDLE const taskHandle, RK_MESG *const mesgPtr);
RK_ERR kMesgWait(RK_TASK_HANDLE const taskHandle,
                 RK_MESG **const mesgPPtr,
                 RK_TICK const timeout);
#if (RK_CONF_ASYNCH_COPY_MESG == ON)
RK_ERR kMesgCopyEndpointInit(RK_TASK_HANDLE const taskHandle);
RK_ERR kMesgSendCopy(RK_TASK_HANDLE const receiverHandle,
                     VOID const *const sendPtr,
                     ULONG const sendBytes);
RK_ERR kMesgRecvCopy(RK_TASK_HANDLE const senderHandle,
                     VOID *const recvPtr,
                     ULONG const recvMaxBytes,
                     ULONG *const recvBytesPtr,
                     RK_TICK const timeout);
#endif
#endif

#if (RK_CONF_SYNCH_MESG == ON)
RK_ERR kSynchMesgInit(RK_TASK_HANDLE const taskHandle,
                      ULONG const requestMaxBytes);
RK_ERR kSynchMesgCall(RK_TASK_HANDLE const serverHandle,
                      RK_SYNCH_ATTR const *const attrPtr,
                      RK_TICK const timeout);
RK_ERR kSynchMesgAccept(RK_SYNCH_CALL_DATA *const callPtr,
                        VOID *const reqPtr,
                        ULONG *const reqBytesPtr,
                        RK_TICK const timeout);
RK_ERR kSynchMesgReply(RK_SYNCH_CALL_DATA const *const callPtr,
                       VOID const *const replyPtr,
                       ULONG const replyBytes);
#endif

#if (RK_CONF_CALLOUT_TIMER == ON)
RK_ERR kTimerCreate(RK_TIMER_HANDLE *const timerHandlePtr,
                    RK_STRING objName,
                    RK_TICK const delay,
                    RK_TICK const period,
                    RK_TIMER_CALLOUT const callout,
                    VOID *const argsPtr,
                    RK_OPTION const opt);
RK_ERR kTimerDestroy(RK_TIMER_HANDLE *const timerHandlePtr);
RK_ERR kTimerCancel(RK_TIMER_HANDLE const timerHandle);
VOID kTimerReload(RK_TIMER_HANDLE const timerHandle, RK_TICK period);
#endif

RK_TICK kTickGet(VOID);
RK_TICK kTickGetMs(VOID);
RK_ERR kSleepDelay(RK_TICK const ticks);
RK_ERR kSleepRelease(RK_TICK const period);
RK_ERR kSleepUntil(RK_TICK *anchorPtr, RK_TICK const period);
RK_ERR kDelay(RK_TICK const ticks);
UINT kGetVersion(VOID);
VOID kGetInfo(const CHAR **infoPPtr);

RK_ERR kMemPartitionInit(RK_MEM_PARTITION *const kobj,
                         VOID *memPoolPtr,
                         ULONG blkSize,
                         const ULONG numBlocks);
VOID *kMemPartitionAlloc(RK_MEM_PARTITION *const kobj);
RK_ERR kMemPartitionFree(RK_MEM_PARTITION *const kobj, VOID *blockPtr);

#if (RK_CONF_MRM == ON)
RK_ERR kMRMCreate(RK_MRM_HANDLE *const mrmHandlePtr,
                  RK_STRING objName,
                  RK_MRM_BUF *const mrmPoolPtr,
                  VOID *mesgPoolPtr,
                  ULONG const nBufs,
                  ULONG const dataSizeWords);
RK_ERR kMRMDestroy(RK_MRM_HANDLE *const mrmHandlePtr);
RK_MRM_BUF *kMRMReserve(RK_MRM_HANDLE const mrmHandle);
RK_ERR kMRMPublish(RK_MRM_HANDLE const mrmHandle,
                   RK_MRM_BUF *const bufPtr,
                   VOID const *dataPtr);
RK_MRM_BUF *kMRMGet(RK_MRM_HANDLE const mrmHandle, VOID *const getMesgPtr);
RK_ERR kMRMUnget(RK_MRM_HANDLE const mrmHandle, RK_MRM_BUF *const bufPtr);
#endif

#ifndef kSleep
#define kSleep(ticks) kSleepDelay(ticks)
#endif

#ifndef kSleepPeriodic
#define kSleepPeriodic(ticks) kSleepRelease(ticks)
#endif

#ifndef kPreemptDisable
#define kPreemptDisable kSchLock
#endif

#ifndef kPreemptEnable
#define kPreemptEnable kSchUnlock
#endif

#ifndef kBusyDelay
#define kBusyDelay(ticks) kDelay(ticks)
#endif

#ifndef RK_TICKS_TO_MS
#define RK_TICKS_TO_MS(ticks) ((ticks) * RK_TICK_INTERVAL_MS)
#endif

#ifndef RK_MS_TO_TICKS_DEFINED
#define RK_MS_TO_TICKS_DEFINED
static inline RK_BOOL K_MS_TO_TICKS_IS_ZERO(RK_TICK t)
{
    return ((t / RK_TICK_INTERVAL_MS) == 0U);
}

static inline RK_TICK RK_MS_TO_TICKS(RK_TICK ms)
{
    if (ms == 0U)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        return (0U);
    }

#if (RK_CONF_ROUND_UP_MS_TO_TICKS == ON)
    if (K_MS_TO_TICKS_IS_ZERO(ms) == RK_TRUE)
    {
        return (1U);
    }
#endif

    return ((RK_TICK)(ms / RK_TICK_INTERVAL_MS));
}
#endif

#ifndef RK_STACK_ALIGN
#define RK_STACK_ALIGN(NWORDS) K_ALIGN((NWORDS) * sizeof(RK_STACK))
#endif

#ifndef RK_APP_RAM_ATTR
#define RK_APP_RAM_ATTR K_ALIGN(4) RK_SECTION_APP_RAM
#endif

#ifndef RK_TASK_STACK_ATTR
#define RK_TASK_STACK_ATTR(NWORDS)                                            \
    RK_STACK_ALIGN(NWORDS) RK_SECTION_TASK_STACK
#endif

#ifndef RK_TASK_HANDLE_ATTR
#define RK_TASK_HANDLE_ATTR RK_SECTION_SHARED_BSS
#endif

#ifndef RK_KOBJ_HANDLE_ATTR
#define RK_KOBJ_HANDLE_ATTR RK_SECTION_SHARED_BSS
#endif

#ifndef RK_DECLARE_LOCAL_KOBJ_HANDLE
#define RK_DECLARE_LOCAL_KOBJ_HANDLE(HANDLE)                                  \
    RK_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_KOBJ_HANDLE
#define RK_DECLARE_GLOBAL_KOBJ_HANDLE(HANDLE)                                 \
    RK_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_KOBJ_HANDLE
#define RK_DECLARE_KOBJ_HANDLE(HANDLE) RK_DECLARE_GLOBAL_KOBJ_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_TASK_HANDLE
#define RK_DECLARE_LOCAL_TASK_HANDLE(HANDLE)                                  \
    RK_TASK_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_TASK_HANDLE
#define RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)                                 \
    RK_TASK_HANDLE HANDLE RK_TASK_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_TASK_HANDLE
#define RK_DECLARE_TASK_HANDLE(HANDLE) RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_MUTEX
#define RK_DECLARE_LOCAL_MUTEX(HANDLE)                                        \
    RK_MUTEX_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MUTEX
#define RK_DECLARE_GLOBAL_MUTEX(HANDLE)                                       \
    RK_MUTEX_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_MUTEX
#define RK_DECLARE_MUTEX(HANDLE) RK_DECLARE_GLOBAL_MUTEX(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_SEMAPHORE
#define RK_DECLARE_LOCAL_SEMAPHORE(HANDLE)                                    \
    RK_SEMAPHORE_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_SEMAPHORE
#define RK_DECLARE_GLOBAL_SEMAPHORE(HANDLE)                                   \
    RK_SEMAPHORE_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_SEMAPHORE
#define RK_DECLARE_SEMAPHORE(HANDLE) RK_DECLARE_GLOBAL_SEMAPHORE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_SLEEP_QUEUE
#define RK_DECLARE_LOCAL_SLEEP_QUEUE(HANDLE)                                  \
    RK_SLEEP_QUEUE_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_SLEEP_QUEUE
#define RK_DECLARE_GLOBAL_SLEEP_QUEUE(HANDLE)                                 \
    RK_SLEEP_QUEUE_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_SLEEP_QUEUE
#define RK_DECLARE_SLEEP_QUEUE(HANDLE) RK_DECLARE_GLOBAL_SLEEP_QUEUE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE
#define RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE(HANDLE)                            \
    RK_MESG_QUEUE_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE
#define RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE(HANDLE)                           \
    RK_MESG_QUEUE_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_MESG_QUEUE_HANDLE
#define RK_DECLARE_MESG_QUEUE_HANDLE(HANDLE)                                  \
    RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_MBOX_HANDLE
#define RK_DECLARE_LOCAL_MBOX_HANDLE(HANDLE)                                  \
    RK_MBOX_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MBOX_HANDLE
#define RK_DECLARE_GLOBAL_MBOX_HANDLE(HANDLE)                                 \
    RK_MBOX_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_MBOX_HANDLE
#define RK_DECLARE_MBOX_HANDLE(HANDLE) RK_DECLARE_GLOBAL_MBOX_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_TIMER
#define RK_DECLARE_LOCAL_TIMER(HANDLE)                                        \
    RK_TIMER_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_TIMER
#define RK_DECLARE_GLOBAL_TIMER(HANDLE)                                       \
    RK_TIMER_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_TIMER
#define RK_DECLARE_TIMER(HANDLE) RK_DECLARE_GLOBAL_TIMER(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_MRM
#define RK_DECLARE_LOCAL_MRM(HANDLE)                                          \
    RK_MRM_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MRM
#define RK_DECLARE_GLOBAL_MRM(HANDLE)                                         \
    RK_MRM_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_MRM
#define RK_DECLARE_MRM(HANDLE) RK_DECLARE_GLOBAL_MRM(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_SHARED_MEM_HANDLE
#define RK_DECLARE_LOCAL_SHARED_MEM_HANDLE(HANDLE)                            \
    RK_SHARED_MEM_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_SHARED_MEM_HANDLE
#define RK_DECLARE_GLOBAL_SHARED_MEM_HANDLE(HANDLE)                           \
    RK_SHARED_MEM_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_SHARED_MEM_HANDLE
#define RK_DECLARE_SHARED_MEM_HANDLE(HANDLE)                                  \
    RK_DECLARE_GLOBAL_SHARED_MEM_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_MEM_PARTITION
#define RK_DECLARE_LOCAL_MEM_PARTITION(PARTITION)                             \
    RK_MEM_PARTITION PARTITION RK_APP_RAM_ATTR;
#endif

#ifndef RK_DECLARE_GLOBAL_MEM_PARTITION
#define RK_DECLARE_GLOBAL_MEM_PARTITION(PARTITION)                            \
    RK_MEM_PARTITION PARTITION K_ALIGN(4) RK_SECTION_SHARED_BSS;
#endif

#ifndef RK_DECLARE_MEM_PARTITION
#define RK_DECLARE_MEM_PARTITION(PARTITION)                                   \
    RK_DECLARE_LOCAL_MEM_PARTITION(PARTITION)
#endif

#ifndef RK_DECLARE_TASK
#define RK_DECLARE_TASK(HANDLE, TASKENTRY, STACKBUF, NWORDS)                  \
    VOID TASKENTRY(VOID *args);                                               \
    RK_STACK STACKBUF[NWORDS] RK_TASK_STACK_ATTR(NWORDS);                     \
    RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

#ifndef RK_MESGQ_MESG_SIZE
#define RK_MESGQ_MESG_SIZE(MESG_TYPE) RK_TYPE_SIZE_POW2_WORDS(MESG_TYPE)
#endif

#ifndef RK_MESGQ_BUF_SIZE
#define RK_MESGQ_BUF_SIZE(MESG_TYPE, N_MESG)                                  \
    (UINT)((RK_MESGQ_MESG_SIZE(MESG_TYPE)) * (N_MESG))
#endif

#ifndef RK_DECLARE_LOCAL_MESG_QUEUE_BUF
#define RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)           \
    ULONG BUFNAME[RK_MESGQ_BUF_SIZE(MESG_TYPE, N_MESG)] K_ALIGN(4)            \
        RK_SECTION_APP_RAM;
#endif

#ifndef RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE
#define RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE(QUEUE_NAME)                        \
    RK_MESG_QUEUE_HANDLE QUEUE_NAME RK_SECTION_APP_RAM = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_LOCAL_MESG_QUEUE
#define RK_DECLARE_LOCAL_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)   \
    RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)               \
    RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE(QUEUE_NAME)
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_QUEUE_BUF
#define RK_DECLARE_GLOBAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)          \
    ULONG BUFNAME[RK_MESGQ_BUF_SIZE(MESG_TYPE, N_MESG)] K_ALIGN(4)            \
        RK_SECTION_SHARED_BSS;
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE
#define RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE(QUEUE_NAME)                       \
    RK_MESG_QUEUE_HANDLE QUEUE_NAME RK_SECTION_SHARED_BSS = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_QUEUE
#define RK_DECLARE_GLOBAL_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)  \
    RK_DECLARE_GLOBAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)              \
    RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE(QUEUE_NAME)
#endif

#ifndef RK_DECLARE_MESG_QUEUE_BUF
#define RK_DECLARE_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)                 \
    RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)
#endif

#ifndef RK_DECLARE_MESG_QUEUE
#define RK_DECLARE_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)         \
    RK_DECLARE_LOCAL_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)
#endif

#ifndef RK_DECLARE_LOCAL_MBOX_BUF
#define RK_DECLARE_LOCAL_MBOX_BUF(BUFNAME, MESG_TYPE)                         \
    RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, 1U)
#endif

#ifndef RK_DECLARE_LOCAL_MBOX_HANDLE
#define RK_DECLARE_LOCAL_MBOX_HANDLE(MBOX_NAME)                               \
    RK_MBOX_HANDLE MBOX_NAME RK_SECTION_APP_RAM = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_LOCAL_MBOX
#define RK_DECLARE_LOCAL_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)                  \
    RK_DECLARE_LOCAL_MBOX_BUF(BUFNAME, MESG_TYPE)                             \
    RK_DECLARE_LOCAL_MBOX_HANDLE(MBOX_NAME)
#endif

#ifndef RK_DECLARE_GLOBAL_MBOX_BUF
#define RK_DECLARE_GLOBAL_MBOX_BUF(BUFNAME, MESG_TYPE)                        \
    RK_DECLARE_GLOBAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, 1U)
#endif

#ifndef RK_DECLARE_GLOBAL_MBOX_HANDLE
#define RK_DECLARE_GLOBAL_MBOX_HANDLE(MBOX_NAME)                              \
    RK_MBOX_HANDLE MBOX_NAME RK_SECTION_SHARED_BSS = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MBOX
#define RK_DECLARE_GLOBAL_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)                 \
    RK_DECLARE_GLOBAL_MBOX_BUF(BUFNAME, MESG_TYPE)                            \
    RK_DECLARE_GLOBAL_MBOX_HANDLE(MBOX_NAME)
#endif

#ifndef RK_DECLARE_MBOX_BUF
#define RK_DECLARE_MBOX_BUF(BUFNAME, MESG_TYPE)                               \
    RK_DECLARE_LOCAL_MBOX_BUF(BUFNAME, MESG_TYPE)
#endif

#ifndef RK_DECLARE_MBOX
#define RK_DECLARE_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)                        \
    RK_DECLARE_LOCAL_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)
#endif

#ifndef kMesgQueueQueryMessageCount
#define kMesgQueueQueryMessageCount(KOBJ, N_MESG_PTR)                         \
    kMesgQueueQuery((KOBJ), (N_MESG_PTR), (NULL), (NULL))
#endif

#ifndef kMesgQueueQueryWaitingReceivers
#define kMesgQueueQueryWaitingReceivers(KOBJ, N_WAIT_R_PTR)                   \
    kMesgQueueQuery((KOBJ), (NULL), (N_WAIT_R_PTR), (NULL))
#endif

#ifndef kMesgQueueQueryWaitingSenders
#define kMesgQueueQueryWaitingSenders(KOBJ, N_WAIT_S_PTR)                     \
    kMesgQueueQuery((KOBJ), (NULL), (NULL), (N_WAIT_S_PTR))
#endif

#ifndef kMboxCreate
#define kMboxCreate kMesgQueueCreate
#endif

#ifndef kMboxDestroy
#define kMboxDestroy kMesgQueueDestroy
#endif

#ifndef kMboxPost
#define kMboxPost kMesgQueueSend
#endif

#ifndef kMboxPend
#define kMboxPend kMesgQueueRecv
#endif

#ifndef kMboxReset
#define kMboxReset kMesgQueueReset
#endif

#ifndef kMboxBroadcast
#define kMboxBroadcast kMesgQueueBroadcast
#endif

#ifndef kMboxBroadcastRecv
#define kMboxBroadcastRecv kMesgQueueBroadcastRecv
#endif

#ifndef RK_MESG_BLOCK_SIZE_BYTES
#define RK_MESG_BLOCK_SIZE_BYTES(MESG_TYPE)                                   \
    ((ULONG)((sizeof(RK_MESG) + sizeof(MESG_TYPE) + RK_WORD_SIZE - 1UL) &     \
             ~(RK_WORD_SIZE - 1UL)))
#endif

#ifndef RK_MESG_POOL_WORDS
#define RK_MESG_POOL_WORDS(MESG_TYPE, N_MESG)                                 \
    ((UINT)((RK_MESG_BLOCK_SIZE_BYTES(MESG_TYPE) / RK_WORD_SIZE) * (N_MESG)))
#endif

#ifndef RK_DECLARE_MESG_POOL_BUF
#define RK_DECLARE_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)                  \
    RK_DECLARE_LOCAL_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)
#endif

#ifndef RK_DECLARE_LOCAL_MESG_POOL_BUF
#define RK_DECLARE_LOCAL_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)            \
    ULONG BUFNAME[RK_MESG_POOL_WORDS(MESG_TYPE, N_MESG)] RK_APP_RAM_ATTR;
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_POOL_BUF
#define RK_DECLARE_GLOBAL_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)           \
    ULONG BUFNAME[RK_MESG_POOL_WORDS(MESG_TYPE, N_MESG)] K_ALIGN(4)           \
        RK_SECTION_SHARED_BSS;
#endif

#ifndef RK_DECLARE_MESG_POOL
#define RK_DECLARE_MESG_POOL(POOL_NAME, BUFNAME, MESG_TYPE, N_MESG)           \
    RK_DECLARE_LOCAL_MESG_POOL(POOL_NAME, BUFNAME, MESG_TYPE, N_MESG)
#endif

#ifndef RK_DECLARE_LOCAL_MESG_POOL
#define RK_DECLARE_LOCAL_MESG_POOL(POOL_NAME, BUFNAME, MESG_TYPE, N_MESG)     \
    RK_DECLARE_LOCAL_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)                \
    RK_DECLARE_LOCAL_MEM_PARTITION(POOL_NAME)
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_POOL
#define RK_DECLARE_GLOBAL_MESG_POOL(POOL_NAME, BUFNAME, MESG_TYPE, N_MESG)    \
    RK_DECLARE_GLOBAL_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)               \
    RK_DECLARE_GLOBAL_MEM_PARTITION(POOL_NAME)
#endif

#ifndef RK_MESG_PAYLOAD
#define RK_MESG_PAYLOAD(MESG_PTR, MESG_TYPE)                                  \
    ((MESG_TYPE *)kMesgPayload((MESG_PTR)))
#endif

#ifndef RK_DECLARE_MEM_POOL
#define RK_DECLARE_MEM_POOL(TYPE, BUFNAME, N_BLOCKS)                          \
    RK_DECLARE_LOCAL_MEM_POOL(TYPE, BUFNAME, N_BLOCKS)
#endif

#ifndef RK_DECLARE_LOCAL_MEM_POOL
#define RK_DECLARE_LOCAL_MEM_POOL(TYPE, BUFNAME, N_BLOCKS)                    \
    ULONG BUFNAME[N_BLOCKS][RK_TYPE_WORD_COUNT(TYPE)] RK_APP_RAM_ATTR;
#endif

#ifndef RK_DECLARE_GLOBAL_MEM_POOL
#define RK_DECLARE_GLOBAL_MEM_POOL(TYPE, BUFNAME, N_BLOCKS)                   \
    ULONG BUFNAME[N_BLOCKS][RK_TYPE_WORD_COUNT(TYPE)] K_ALIGN(4)              \
        RK_SECTION_SHARED_BSS;
#endif

#ifdef __cplusplus
}
#endif

#endif /* RK_API_APP_H */
