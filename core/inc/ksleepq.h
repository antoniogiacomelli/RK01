/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_SLEEPQ_H
#define RK_SLEEPQ_H
#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kobjs.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (RK_CONF_SLEEP_QUEUE == ON)
RK_ERR kSleepQueueInit(RK_SLEEP_QUEUE* const);

RK_ERR kSleepQueueCreate(RK_SLEEP_QUEUE_HANDLE *const, RK_STRING);
RK_ERR kSleepQueueCreateGlobalScope(RK_SLEEP_QUEUE_HANDLE *const, RK_STRING);
RK_ERR kSleepQueueCreateDomainScope(RK_SLEEP_QUEUE_HANDLE *const, RK_STRING,
                                    RK_DOMAIN *const);
RK_ERR kSleepQueueDestroy(RK_SLEEP_QUEUE_HANDLE *const);

RK_ERR kSleepQueueSleep(RK_SLEEP_QUEUE_HANDLE const, RK_TICK const);
RK_ERR kSleepQueueSignal(RK_SLEEP_QUEUE_HANDLE const);
RK_ERR kSleepQueueReady(RK_SLEEP_QUEUE_HANDLE const, RK_TASK_HANDLE);
RK_ERR kSleepQueueUnready(RK_SLEEP_QUEUE_HANDLE const, RK_TASK_HANDLE);
RK_ERR kSleepQueueQuery(RK_SLEEP_QUEUE_HANDLE const, ULONG* const);
RK_ERR kSleepQueueWake(RK_SLEEP_QUEUE_HANDLE const, UINT, UINT*);

#ifndef kSleepQueueFlush
#define kSleepQueueFlush(o) kSleepQueueWake(o, 0, NULL)
#endif
#if (RK_CONF_CONDVAR == ON)
RK_ERR kCondVarInit(RK_SLEEP_QUEUE *const, RK_MUTEX *const);
RK_ERR kCondVarWait(RK_SLEEP_QUEUE_HANDLE const, RK_MUTEX_HANDLE const,
                    RK_TICK const);
RK_ERR kCondVarSignal(RK_SLEEP_QUEUE_HANDLE const);
RK_ERR kCondVarBroadcast(RK_SLEEP_QUEUE_HANDLE const);
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif
