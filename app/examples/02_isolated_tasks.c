/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Advanced example 02: isolated one-task domains exchanging copied messages.
 */

#include <kapi_domain.h>

#define APP_LOG_PRIO (10U)
#define ISO_STACK_WORDS (256U)
#define ALPHA_PRIO (8U)
#define BETA_PRIO (9U)

typedef struct
{
    ULONG seq;
    ULONG value;
} IsoMsg;

RK_DECLARE_ISOLATED_TASK(alphaHandle, AlphaTask, alphaStack, ISO_STACK_WORDS)
RK_DECLARE_ISOLATED_TASK(betaHandle, BetaTask, betaStack, ISO_STACK_WORDS)

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

    AppCheck_(kTaskInitIsolated(&alphaHandle, AlphaTask, RK_NO_ARGS,
                                "Alpha", alphaStack, ISO_STACK_WORDS,
                                ALPHA_PRIO, RK_PREEMPT));
    AppCheck_(kTaskInitIsolated(&betaHandle, BetaTask, RK_NO_ARGS,
                                "Beta", betaStack, ISO_STACK_WORDS,
                                BETA_PRIO, RK_PREEMPT));

    AppCheck_(kMesgCopyEndpointInit(alphaHandle));
    AppCheck_(kMesgCopyEndpointInit(betaHandle));
}

VOID AlphaTask(VOID *args)
{
    ULONG seq = 0UL;

    RK_UNUSEARGS

    while (1)
    {
        IsoMsg msg;
        ULONG rxBytes = 0UL;

        seq++;
        msg.seq = seq;
        msg.value = seq * 10UL;

        AppCheck_(kMesgSendCopy(betaHandle, &msg, sizeof(msg)));
        AppCheck_(kMesgRecvCopy(betaHandle, &msg, sizeof(msg),
                                &rxBytes, RK_WAIT_FOREVER));
        K_ASSERT(rxBytes == sizeof(msg));

        kLog("alpha reply seq=%lu value=%lu", msg.seq, msg.value);
        kSleep(RK_MS_TO_TICKS(1000UL));
    }
}

VOID BetaTask(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        IsoMsg msg;
        ULONG rxBytes = 0UL;

        AppCheck_(kMesgRecvCopy(alphaHandle, &msg, sizeof(msg),
                                &rxBytes, RK_WAIT_FOREVER));
        K_ASSERT(rxBytes == sizeof(msg));

        msg.value++;
        AppCheck_(kMesgSendCopy(alphaHandle, &msg, sizeof(msg)));
    }
}
