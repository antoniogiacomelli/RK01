/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Advanced example 07: signal delivery on return to an unprivileged task.
 * A pending signal runs a user handler at a kernel-to-user boundary, then the
 * worker resumes its original context.
 */

#include <kapi_domain.h>

#define APP_LOG_PRIO (10U)
#define SIGNAL_DOMAIN_BYTES (1024U)
#define TASK_STACK_WORDS (256U)
#define SIGNAL_ALT_STACK_WORDS (128U)
#define TASK1_PERIOD_MS (500UL)
#define TASK2_PERIOD_MS (500UL)
#define SIGNAL_EVERY_RUNS (10UL)
#define TASK1_PRIO (9U)
#define TASK2_PRIO (8U)
#define DEMO_SIGNAL RK_SIGNAL_1

RK_DECLARE_DOMAIN_RAM(SignalDemoState,
                      RK_DOMAIN_RAM_ARRAY(RK_STACK, signalStack,
                                          SIGNAL_ALT_STACK_WORDS)
                          RK_DOMAIN_RAM_MEMBER(ULONG, signalCount)
                              RK_DOMAIN_RAM_MEMBER(ULONG, sentCount)
                                  RK_DOMAIN_RAM_MEMBER(ULONG, task1RunCount)
                                      RK_DOMAIN_RAM_MEMBER(ULONG,
                                                           task2RunCount))

RK_DECLARE_TYPED_DOMAIN(signalDomain, signalRam, SignalDemoState,
                        SIGNAL_DOMAIN_BYTES)
RK_DECLARE_DOMAIN_TASK(signalTask1Handle, SignalTask1)
RK_DECLARE_DOMAIN_TASK(signalTask2Handle, SignalTask2)
RK_DECLARE_DOMAIN_TASK_STACK(signalTask1Stack, TASK_STACK_WORDS)
RK_DECLARE_DOMAIN_TASK_STACK(signalTask2Stack, TASK_STACK_WORDS)

static VOID SignalHandler_(RK_SIGNAL const signal)
{
    SignalDemoState *const statePtr = RK_DOMAIN_STATE(signalRam);

    if (signal == DEMO_SIGNAL)
    {
        statePtr->signalCount++;
    }
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
    kLogInit(APP_LOG_PRIO);

    {
        RK_ERR err = RK_DOMAIN_INIT_TYPED(&signalDomain, signalRam, "SigD");
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    SignalDemoState *const statePtr = RK_DOMAIN_STATE(signalRam);

    {
        RK_ERR err = kTaskInitDomain(&signalTask1Handle, SignalTask1,
                                     statePtr, "Sig1", signalTask1Stack,
                                     TASK_STACK_WORDS, TASK1_PRIO, RK_PREEMPT,
                                     &signalDomain);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    {
        RK_ERR err = kTaskInitDomain(&signalTask2Handle, SignalTask2,
                                     statePtr, "Sig2", signalTask2Stack,
                                     TASK_STACK_WORDS, TASK2_PRIO, RK_PREEMPT,
                                     &signalDomain);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
}

VOID SignalTask1(VOID *args)
{
    SignalDemoState *const statePtr = (SignalDemoState *)args;

    K_ASSERT(statePtr != NULL);
    {
        RK_ERR err = kSignalHandlerSet(DEMO_SIGNAL, SignalHandler_,
                                       statePtr->signalStack,
                                       sizeof(statePtr->signalStack));
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    while (1)
    {
        statePtr->task1RunCount++;
        kLog("signal task1 run=%lu signals=%lu",
             statePtr->task1RunCount, statePtr->signalCount);

        {
            RK_ERR err = kSleepRelease(RK_MS_TO_TICKS(TASK1_PERIOD_MS));
            K_ASSERT((err == RK_ERR_SUCCESS) ||
                     (err == RK_ERR_SIGNAL_INTERRUPTED));
        }
    }
}

VOID SignalTask2(VOID *args)
{
    SignalDemoState *const statePtr = (SignalDemoState *)args;

    K_ASSERT(statePtr != NULL);

    while (1)
    {
        statePtr->task2RunCount++;
        kLog("signal task2 run=%lu", statePtr->task2RunCount);
        if ((statePtr->task2RunCount % SIGNAL_EVERY_RUNS) == 0UL)
        {
            statePtr->sentCount++;
            kLog("signal task2 will signal sent=%lu", statePtr->sentCount);
            {
                RK_ERR err = kSignalSend(signalTask1Handle, DEMO_SIGNAL);
                K_ASSERT(err == RK_ERR_SUCCESS);
            }
        }

        {
            RK_ERR err = kSleepRelease(RK_MS_TO_TICKS(TASK2_PERIOD_MS));
            K_ASSERT(err == RK_ERR_SUCCESS);
        }
    }
}
