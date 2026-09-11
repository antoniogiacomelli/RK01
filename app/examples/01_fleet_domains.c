/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Advanced example 01: two explicit domains crossing the boundary with copied
 * direct messages.
 */

#include <kapi_domain.h>

#define APP_LOG_PRIO (10U)
#define DOMAIN_BYTES (2048U)
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

RK_DECLARE_DOMAIN(fleetControlDomain, fleetControlRam, DOMAIN_BYTES)
RK_DECLARE_DOMAIN(fleetCommsDomain, fleetCommsRam, DOMAIN_BYTES)
RK_DECLARE_DOMAIN_TASK(fleetPlannerHandle, FleetPlannerTask)
RK_DECLARE_DOMAIN_TASK(fleetLinkHandle, FleetLinkTask)
RK_DECLARE_DOMAIN_TASK_STACK(fleetPlannerStack, TASK_STACK_WORDS)
RK_DECLARE_DOMAIN_TASK_STACK(fleetLinkStack, TASK_STACK_WORDS)

static FleetControlState *fleetControlState;
static FleetCommsState *fleetCommsState;

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
        RK_ERR err = kDomainInit(&fleetControlDomain, fleetControlRam,
                                 sizeof(fleetControlRam), "FleetC");
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    {
        RK_ERR err = kDomainInit(&fleetCommsDomain, fleetCommsRam,
                                 sizeof(fleetCommsRam), "FleetM");
        K_ASSERT(err == RK_ERR_SUCCESS);
    }

    fleetControlState = RK_DOMAIN_ALLOC(&fleetControlDomain, FleetControlState);
    K_ASSERT(fleetControlState != NULL);

    fleetCommsState = RK_DOMAIN_ALLOC(&fleetCommsDomain, FleetCommsState);
    K_ASSERT(fleetCommsState != NULL);

    {
        RK_ERR err = kTaskInitDomain(
            &fleetPlannerHandle, FleetPlannerTask, fleetControlState, "Plan",
            fleetPlannerStack, TASK_STACK_WORDS, PLANNER_PRIO, RK_PREEMPT,
            &fleetControlDomain);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    {
        RK_ERR err =
            kTaskInitDomain(&fleetLinkHandle, FleetLinkTask, fleetCommsState,
                            "Link", fleetLinkStack, TASK_STACK_WORDS, LINK_PRIO,
                            RK_PREEMPT, &fleetCommsDomain);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    {
        RK_ERR err = kMesgCopyEndpointInit(fleetLinkHandle);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
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

        {
            RK_ERR err = kMesgSendCopy(fleetLinkHandle, &order, sizeof(order));
            K_ASSERT(err == RK_ERR_SUCCESS);
        }
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

        {
            RK_ERR err = kMesgRecvCopy(RK_ANY_TASK, &order, sizeof(order),
                                       &rxBytes, RK_WAIT_FOREVER);
            K_ASSERT(err == RK_ERR_SUCCESS);
        }
        K_ASSERT(rxBytes == sizeof(order));
        K_ASSERT(order.checksum == (order.seq ^ order.demand));

        statePtr->lastSeq = order.seq;
        statePtr->lastDemand = order.demand;
        statePtr->rxCount++;

        kLog("fleet link rx=%lu seq=%lu demand=%lu", statePtr->rxCount,
             statePtr->lastSeq, statePtr->lastDemand);
    }
}
