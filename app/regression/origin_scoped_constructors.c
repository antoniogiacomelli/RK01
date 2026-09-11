/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Origin regression: explicit-scope constructors are BOOT/trusted construction
 * APIs. Even if an ordinary unprivileged task reaches their symbols, they must
 * fail with RK_ERR_INVALID_PHASE before entering direct object construction.
 */

#include <kapi_trusted.h>
#include <klogger.h>

#define ORIGIN_TASK_STACK_WORDS (256U)
#define ORIGIN_TASK_PRIO (8U)

RK_DECLARE_TASK(originTaskHandle, OriginTask, originTaskStack,
                ORIGIN_TASK_STACK_WORDS)

static volatile ULONG originChecks RK_SHARED_RAM_ATTR;

static inline VOID OriginExpectInvalidPhase_(RK_ERR const err)
{
    K_ASSERT(err == RK_ERR_INVALID_PHASE);
    if (err != RK_ERR_INVALID_PHASE)
    {
        while (1)
        {
            kErrHandler((RK_FAULT)err);
        }
    }

    originChecks++;
}

int main(void)
{
    kCoreInit();
    kInit();

    while (1)
    {
        kErrHandler(RK_FAULT_APP_CRASH);
    }
}

VOID kApplicationInit(VOID)
{
    kLogInit(10U);

    {
        RK_ERR const err = kTaskInit(&originTaskHandle, OriginTask, RK_NO_ARGS,
                                     "Orig", originTaskStack,
                                     ORIGIN_TASK_STACK_WORDS, ORIGIN_TASK_PRIO,
                                     RK_PREEMPT);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
}

VOID OriginTask(VOID *args)
{
    RK_UNUSEARGS

#if (RK_CONF_MUTEX == ON)
    OriginExpectInvalidPhase_(
        kMutexCreateGlobalScope(NULL, "MxG", RK_PRIO_INHERITANCE));
    OriginExpectInvalidPhase_(
        kMutexCreateDomainScope(NULL, "MxD", RK_PRIO_INHERITANCE, NULL));
#endif

#if (RK_CONF_SEMAPHORE == ON)
    OriginExpectInvalidPhase_(
        kSemaphoreCreateGlobalScope(NULL, "SeG", 0U, 1U));
    OriginExpectInvalidPhase_(
        kSemaphoreCreateDomainScope(NULL, "SeD", 0U, 1U, NULL));
#endif

#if (RK_CONF_SLEEP_QUEUE == ON)
    OriginExpectInvalidPhase_(kSleepQueueCreateGlobalScope(NULL, "SqG"));
    OriginExpectInvalidPhase_(kSleepQueueCreateDomainScope(NULL, "SqD", NULL));
#endif

#if (RK_CONF_MESG_QUEUE == ON)
    OriginExpectInvalidPhase_(
        kMesgQueueCreateGlobalScope(NULL, "MqG", NULL, 1UL, 1UL));
    OriginExpectInvalidPhase_(
        kMesgQueueCreateDomainScope(NULL, "MqD", NULL, 1UL, 1UL, NULL));
#endif

#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
    OriginExpectInvalidPhase_(
        kMesgPoolInitGlobalScope(NULL, NULL, 1UL, 1UL, 1U));
    OriginExpectInvalidPhase_(
        kMesgPoolInitDomainScope(NULL, NULL, 1UL, 1UL, 1U, NULL));
#endif

#if (RK_CONF_MRM == ON)
    OriginExpectInvalidPhase_(
        kMRMCreateDomainScope(NULL, "MRM", NULL, NULL, 1UL, 1UL, NULL));
#endif

#if (RK_CONF_CALLOUT_TIMER == ON)
    OriginExpectInvalidPhase_(
        kTimerCreateGlobalScope(NULL, "TiG", 1UL, 1UL, NULL, NULL,
                                RK_TIMER_RELOAD));
    OriginExpectInvalidPhase_(
        kTimerCreateDomainScope(NULL, "TiD", 1UL, 1UL, NULL, NULL,
                                RK_TIMER_RELOAD, NULL));
#endif

    kLog("origin scoped constructor regression passed checks=%lu",
         originChecks);

    while (1)
    {
        (VOID)kSleepDelay(RK_MS_TO_TICKS(1000UL));
    }
}
