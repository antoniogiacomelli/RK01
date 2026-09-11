/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_MESGQ_H
#define RK_MESGQ_H

#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kobjs.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (RK_CONF_MESG_QUEUE == ON)
#define RK_MESGQ_RECV_WAIT_NORMAL ((UINT)0x0)
#define RK_MESGQ_RECV_WAIT_BROADCAST ((UINT)0xB001)
#define RK_MESGQ_RECV_BROADCAST_DELIVER ((UINT)0xB002)
#define RK_MESGQ_RECV_DIRECT_DELIVER ((UINT)0xD001)

RK_ERR kMesgQueueInit(RK_MESG_QUEUE *const, VOID *const, ULONG const,
                      ULONG const);
RK_ERR kMesgQueueCreate(RK_MESG_QUEUE_HANDLE *const, RK_STRING, VOID *const,
                        ULONG const, ULONG const);
RK_ERR kMesgQueueCreateGlobalScope(RK_MESG_QUEUE_HANDLE *const, RK_STRING,
                                   VOID *const, ULONG const, ULONG const);
RK_ERR kMesgQueueCreateDomainScope(RK_MESG_QUEUE_HANDLE *const, RK_STRING,
                                   VOID *const, ULONG const, ULONG const,
                                   RK_DOMAIN *const);
RK_ERR kMesgQueueDestroy(RK_MESG_QUEUE_HANDLE *const);
#ifndef kMboxCreate
#define kMboxCreate kMesgQueueCreate
#endif
#ifndef kMboxDestroy
#define kMboxDestroy kMesgQueueDestroy
#endif
RK_ERR kMesgQueueSend(RK_MESG_QUEUE_HANDLE const, VOID *const, RK_TICK const);
RK_ERR kMesgQueueRecv(RK_MESG_QUEUE_HANDLE const, VOID *const, RK_TICK const);
RK_ERR kMesgQueuePeek(RK_MESG_QUEUE_HANDLE const, VOID *const);
RK_ERR kMesgQueueReset(RK_MESG_QUEUE_HANDLE const);
RK_ERR kMesgQueueQuery(RK_MESG_QUEUE_HANDLE const, UINT *const,
                       UINT *const, UINT *const);
#ifndef kMesgQueueQueryMessageCount
#define kMesgQueueQueryMessageCount(KOBJ, N_MESG_PTR)\
        kMesgQueueQuery((KOBJ), (N_MESG_PTR), (NULL), (NULL))
#endif
#ifndef kMesgQueueQueryWaitingReceivers
#define kMesgQueueQueryWaitingReceivers(KOBJ, N_WAIT_R_PTR)\
        kMesgQueueQuery((KOBJ), (NULL), (N_WAIT_R_PTR), (NULL))
#endif
#ifndef kMesgQueueQueryWaitingSenders
#define kMesgQueueQueryWaitingSenders(KOBJ, N_WAIT_S_PTR)\
        kMesgQueueQuery((KOBJ), (NULL), (NULL), (N_WAIT_S_PTR))
#endif
#ifndef kMboxQuery
#define kMboxQuery kMesgQueueQuery
#endif
#ifndef kMboxQueryMessageCount
#define kMboxQueryMessageCount(KOBJ, N_MESG_PTR)\
        kMesgQueueQueryMessageCount((KOBJ), (N_MESG_PTR))
#endif
#ifndef kMboxQueryWaitingReceivers
#define kMboxQueryWaitingReceivers(KOBJ, N_WAIT_R_PTR)\
        kMesgQueueQueryWaitingReceivers((KOBJ), (N_WAIT_R_PTR))
#endif
#ifndef kMboxQueryWaitingSenders
#define kMboxQueryWaitingSenders(KOBJ, N_WAIT_S_PTR)\
        kMesgQueueQueryWaitingSenders((KOBJ), (N_WAIT_S_PTR))
#endif
RK_ERR kMesgQueueJam(RK_MESG_QUEUE_HANDLE const queueHandle,
                     VOID *const sendPtr,
                     const RK_TICK timeout);
RK_ERR kMesgQueuePostOvw(RK_MESG_QUEUE_HANDLE const queueHandle,
                         VOID *sendPtr);
RK_ERR kMesgQueueBroadcast(RK_MESG_QUEUE_HANDLE const queueHandle,
                           VOID *const sendPtr, UINT *const nRecvPtr);
RK_ERR kMesgQueueBroadcastWake(RK_MESG_QUEUE *const kobj, UINT const nTasks);
RK_ERR kMesgQueueBroadcastRecv(RK_MESG_QUEUE_HANDLE const queueHandle,
                               VOID *const recvPtr,
                               const RK_TICK timeout);

#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)

RK_ERR kMesgQueueInstallSendCbk(RK_MESG_QUEUE_HANDLE const queueHandle,
                                VOID (*cbk)(RK_MESG_QUEUE *));
#endif

/* Message Queue Helpers */
#ifndef RK_MESGQ_MESG_SIZE
#define RK_MESGQ_MESG_SIZE(MESG_TYPE)\
        RK_TYPE_SIZE_POW2_WORDS(MESG_TYPE)
#ifndef RK_MBOX_MESG_SIZE
#define RK_MBOX_MESG_SIZE(MESG_TYPE) RK_MESGQ_MESG_SIZE(MESG_TYPE)
#endif
#endif

#ifndef RK_MESGQ_BUF_SIZE
#define RK_MESGQ_BUF_SIZE(MESG_TYPE, N_MESG)\
        (UINT)((RK_MESGQ_MESG_SIZE(MESG_TYPE)) * (N_MESG))
#endif
/**
 * @brief Declares the appropriate buffer to be used
 *        by a Message Queue.
 * @param BUFNAME Name of the array.
 * @param MESG_TYPE Type of the message.
 * @param N_MESG   Number of messages
 *
 */
#ifndef RK_DECLARE_LOCAL_MESG_QUEUE_BUF
#define RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)          \
    ULONG BUFNAME[RK_MESGQ_BUF_SIZE(MESG_TYPE, N_MESG)] K_ALIGN(4)           \
        RK_SECTION_APP_RAM;
#endif

#ifndef RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE
#define RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE(QUEUE_NAME)                       \
    RK_MESG_QUEUE_HANDLE QUEUE_NAME RK_SECTION_APP_RAM = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_LOCAL_MESG_QUEUE
#define RK_DECLARE_LOCAL_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)  \
    RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)              \
    RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE(QUEUE_NAME)
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_QUEUE_BUF
#define RK_DECLARE_GLOBAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)         \
    ULONG BUFNAME[RK_MESGQ_BUF_SIZE(MESG_TYPE, N_MESG)] K_ALIGN(4)           \
        RK_SECTION_SHARED_BSS;
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE
#define RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE(QUEUE_NAME)                      \
    RK_MESG_QUEUE_HANDLE QUEUE_NAME RK_SECTION_SHARED_BSS = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_QUEUE
#define RK_DECLARE_GLOBAL_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG) \
    RK_DECLARE_GLOBAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)             \
    RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE(QUEUE_NAME)
#endif

#ifndef RK_DECLARE_MESG_QUEUE_BUF
#define RK_DECLARE_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)                \
    RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)
#endif

#ifndef RK_DECLARE_MESG_QUEUE
#define RK_DECLARE_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)        \
    RK_DECLARE_LOCAL_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)
#endif

#ifndef RK_DECLARE_LOCAL_MBOX_BUF
#define RK_DECLARE_LOCAL_MBOX_BUF(BUFNAME, MESG_TYPE)                        \
    RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, 1U)
#endif

#ifndef RK_DECLARE_LOCAL_MBOX_HANDLE
#define RK_DECLARE_LOCAL_MBOX_HANDLE(MBOX_NAME)                              \
    RK_MBOX_HANDLE MBOX_NAME RK_SECTION_APP_RAM = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_LOCAL_MBOX
#define RK_DECLARE_LOCAL_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)                 \
    RK_DECLARE_LOCAL_MBOX_BUF(BUFNAME, MESG_TYPE)                            \
    RK_DECLARE_LOCAL_MBOX_HANDLE(MBOX_NAME)
#endif

#ifndef RK_DECLARE_GLOBAL_MBOX_BUF
#define RK_DECLARE_GLOBAL_MBOX_BUF(BUFNAME, MESG_TYPE)                       \
    RK_DECLARE_GLOBAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, 1U)
#endif

#ifndef RK_DECLARE_GLOBAL_MBOX_HANDLE
#define RK_DECLARE_GLOBAL_MBOX_HANDLE(MBOX_NAME)                             \
    RK_MBOX_HANDLE MBOX_NAME RK_SECTION_SHARED_BSS = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MBOX
#define RK_DECLARE_GLOBAL_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)                \
    RK_DECLARE_GLOBAL_MBOX_BUF(BUFNAME, MESG_TYPE)                           \
    RK_DECLARE_GLOBAL_MBOX_HANDLE(MBOX_NAME)
#endif

#ifndef RK_DECLARE_MBOX_BUF
#define RK_DECLARE_MBOX_BUF(BUFNAME, MESG_TYPE)                              \
    RK_DECLARE_LOCAL_MBOX_BUF(BUFNAME, MESG_TYPE)
#endif

#ifndef RK_DECLARE_MBOX
#define RK_DECLARE_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)                       \
    RK_DECLARE_LOCAL_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)
#endif

#endif /* RK_CONF_MESG_QUEUE */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* RK_MESGQ_H */
