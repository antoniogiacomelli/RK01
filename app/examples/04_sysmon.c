/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Advanced example 04: opt-in SysMon diagnostics profile.
 */

#include <kapi_app.h>
#include <kapi_diag.h>

#define APP_LOG_PRIO (10U)
#define TASK_STACK_WORDS (256U)
#define PRODUCER_PRIO (8U)
#define CONSUMER_PRIO (9U)

typedef struct
{
    ULONG seq;
    ULONG value;
} MonitorState;

static MonitorState monitorState;
static RK_DECLARE_SEMAPHORE(updateSema)

RK_DECLARE_TASK(producerHandle, ProducerTask, producerStack, TASK_STACK_WORDS)
RK_DECLARE_TASK(consumerHandle, ConsumerTask, consumerStack, TASK_STACK_WORDS)

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
        RK_ERR err = kSemaphoreCreate(&updateSema, "Update", 0U, 1U);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }

    {
        RK_ERR err = kTaskInit(&producerHandle, ProducerTask, RK_NO_ARGS,
                               "Prod", producerStack, TASK_STACK_WORDS,
                               PRODUCER_PRIO, RK_PREEMPT);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    {
        RK_ERR err = kTaskInit(&consumerHandle, ConsumerTask, RK_NO_ARGS,
                               "Cons", consumerStack, TASK_STACK_WORDS,
                               CONSUMER_PRIO, RK_PREEMPT);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }

    {
        RK_ERR err = kSysMonInit();
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
}

VOID ProducerTask(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        monitorState.seq++;
        monitorState.value = (monitorState.seq * 7UL) & 0xFFUL;

        kSemaphorePost(updateSema);
        kLog("sysmon demo seq=%lu value=%lu", monitorState.seq,
             monitorState.value);
        kSleep(RK_MS_TO_TICKS(1000UL));
    }
}

VOID ConsumerTask(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        {
            RK_ERR err = kSemaphorePend(updateSema, RK_WAIT_FOREVER);
            K_ASSERT(err == RK_ERR_SUCCESS);
        }
        kLog("consumer observed seq=%lu value=%lu", monitorState.seq,
             monitorState.value);
    }
}
