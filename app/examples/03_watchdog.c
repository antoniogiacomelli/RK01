/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Advanced example 03: board watchdog policy. STM32F401RE uses the HAL
 * watchdog; targets without one use a visible heartbeat task.
 */

#include <kapi_app.h>
#include <khal.h>

#define APP_LOG_PRIO (10U)
#define HEARTBEAT_STACK_WORDS (256U)
#define HEARTBEAT_PRIO (12U)

#if (K_HAL_HAS_WATCHDOG == 0U)
RK_DECLARE_TASK(heartbeatHandle, HeartbeatTask, heartbeatStack,
                HEARTBEAT_STACK_WORDS)
#else
RK_DECLARE_TIMER(watchdogTimer)
#endif

RK_FORCE_INLINE
static inline VOID AppCheck_(RK_ERR const err)
{
    K_ASSERT(err == RK_ERR_SUCCESS);
    if (err != RK_ERR_SUCCESS)
    {
        while (1)
        {
            kErrHandler((RK_FAULT)err);
        }
    }
}

#if (K_HAL_HAS_WATCHDOG == 1U)
static VOID WatchdogKick_(VOID *args)
{
    RK_UNUSEARGS

    kHalWatchdogKick();
}
#endif

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
    kLogInit(APP_LOG_PRIO);

#if (K_HAL_HAS_WATCHDOG == 1U)
    kHalWatchdogConfigure();
    AppCheck_(kTimerCreate(&watchdogTimer, "WdgTmr", 0UL,
                           RK_MS_TO_TICKS(K_HAL_WATCHDOG_FEED_MS),
                           WatchdogKick_, RK_NO_ARGS, RK_TIMER_RELOAD));
#else
    AppCheck_(kTaskInit(&heartbeatHandle, HeartbeatTask, RK_NO_ARGS,
                        "Beat", heartbeatStack, HEARTBEAT_STACK_WORDS,
                        HEARTBEAT_PRIO, RK_PREEMPT));
#endif
}

#if (K_HAL_HAS_WATCHDOG == 0U)
VOID HeartbeatTask(VOID *args)
{
    ULONG beat = 0UL;

    RK_UNUSEARGS

    while (1)
    {
        beat++;
        kLog("heartbeat beat=%lu", beat);
        kSleep(RK_MS_TO_TICKS(1000UL));
    }
}
#endif
