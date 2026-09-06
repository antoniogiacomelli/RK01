/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Advanced example 01: two explicit modules crossing the boundary with copied
 * direct messages.
 */

#include <kapi_module.h>

#define APP_LOG_PRIO (10U)
#define MODULE_BYTES (2048U)
#define TASK_STACK_WORDS (256U)
#define PLANNER_PRIO (8U)
#define LINK_PRIO (9U)

typedef struct
{
    ULONG seq;
    ULONG demand;
} FleetControlState;

typedef struct
{
    ULONG lastSeq;
    ULONG lastDemand;
    ULONG rxCount;
} FleetCommsState;

typedef struct
{
    ULONG seq;
    ULONG demand;
    ULONG checksum;
} FleetOrder;

RK_DECLARE_MODULE(fleetControlModule, fleetControlRam, MODULE_BYTES)
RK_DECLARE_MODULE(fleetCommsModule, fleetCommsRam, MODULE_BYTES)
RK_DECLARE_MODULE_TASK(fleetPlannerHandle, FleetPlannerTask)
RK_DECLARE_MODULE_TASK(fleetLinkHandle, FleetLinkTask)

static FleetControlState *fleetControlState;
static FleetCommsState *fleetCommsState;

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

RK_FORCE_INLINE
static inline VOID *AppCheckPtr_(VOID *const ptr)
{
    K_ASSERT(ptr != NULL);
    if (ptr == NULL)
    {
        while (1)
        {
            kErrHandler(RK_FAULT_INVALID_PARAM);
        }
    }

    return (ptr);
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

    AppCheck_(kModuleInit(&fleetControlModule, fleetControlRam,
                          sizeof(fleetControlRam), "FleetC"));
    AppCheck_(kModuleInit(&fleetCommsModule, fleetCommsRam,
                          sizeof(fleetCommsRam), "FleetM"));

    fleetControlState =
        AppCheckPtr_(RK_MODULE_ALLOC(&fleetControlModule,
                                     FleetControlState));
    fleetCommsState =
        AppCheckPtr_(RK_MODULE_ALLOC(&fleetCommsModule, FleetCommsState));

    AppCheck_(kModuleTaskInit(&fleetControlModule, &fleetPlannerHandle,
                              FleetPlannerTask, fleetControlState,
                              "Plan", TASK_STACK_WORDS,
                              PLANNER_PRIO, RK_PREEMPT));
    AppCheck_(kModuleTaskInit(&fleetCommsModule, &fleetLinkHandle,
                              FleetLinkTask, fleetCommsState,
                              "Link", TASK_STACK_WORDS,
                              LINK_PRIO, RK_PREEMPT));
    AppCheck_(kMesgCopyEndpointInit(fleetLinkHandle));
}

VOID FleetPlannerTask(VOID *args)
{
    FleetControlState *const statePtr = (FleetControlState *)args;

    K_ASSERT(statePtr != NULL);

    while (1)
    {
        FleetOrder order;

        statePtr->seq++;
        statePtr->demand = 20UL + ((statePtr->seq * 3UL) % 30UL);

        order.seq = statePtr->seq;
        order.demand = statePtr->demand;
        order.checksum = order.seq ^ order.demand;

        AppCheck_(kMesgSendCopy(fleetLinkHandle, &order, sizeof(order)));
        kLog("fleet plan seq=%lu demand=%lu", order.seq, order.demand);
        kSleep(RK_MS_TO_TICKS(1000UL));
    }
}

VOID FleetLinkTask(VOID *args)
{
    FleetCommsState *const statePtr = (FleetCommsState *)args;

    K_ASSERT(statePtr != NULL);

    while (1)
    {
        FleetOrder order;
        ULONG rxBytes = 0UL;

        AppCheck_(kMesgRecvCopy(RK_ANY_TASK, &order, sizeof(order),
                                &rxBytes, RK_WAIT_FOREVER));
        K_ASSERT(rxBytes == sizeof(order));
        K_ASSERT(order.checksum == (order.seq ^ order.demand));

        statePtr->lastSeq = order.seq;
        statePtr->lastDemand = order.demand;
        statePtr->rxCount++;

        kLog("fleet link rx=%lu seq=%lu demand=%lu",
             statePtr->rxCount, statePtr->lastSeq, statePtr->lastDemand);
    }
}
