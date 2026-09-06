/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/****************************************************************************************

RK01 APPLICATION DEMONSTRATION

The default shape is now the RK0-shaped one: tasks created with kTaskInit()
join the implicit App module. They share App-module RAM and can use ordinary
module-local synchronisers without declaring an RK_MODULE by hand.

+------------------------------------------------------------------------------+
| Implicit App module                                                          |
|                                                                              |
|  DebugHeartbeatTask (when no HAL watchdog is available)                      |
|                                                                              |
|  PlantSensorTask -- copied PlantSample --> PlantControllerTask               |
|                                                |                             |
|                                                | locks plantStateMutex       |
|                                                | updates plantState          |
|                                                v                             |
|                                      posts plantActuatorSema                 |
|                                                |                             |
|                                                v                             |
|                                      PlantActuatorTask                       |
|                                                                              |
+------------------------------------------------------------------------------+

+--------------------+       copied msg       +--------------------+
| IsoAlphaTask       | ---------------------> | IsoBetaTask        |
| isolated module    | <--------------------- | isolated module    |
+--------------------+       copied reply     +--------------------+
          |
          | copied msg / copied reply
          v
+--------------------+
| IsoGammaTask       |
| isolated module    |
+--------------------+

+----------------------------+        copied queue        +----------------------------+
| Fleet Control Module       | -------------------------> | Fleet Comms Module         |
| fleetControlModule         |       FleetOrder           | fleetCommsModule           |
|                            |                            |                            |
| FleetPlannerTask           |                            | FleetLinkTxTask            |
| owns desired control state |                            | owns transmitted status    |
|                            |                            |                            |
| FleetTelemetryTask         | -- copied call/reply ----> | FleetSupervisorTask        |
| asks for status            | <--- copied reply -------- | replies from comms state   |
+----------------------------+                            +----------------------------+

Task roles:

o DebugHeartbeatTask
Logs once per second on targets without a HAL watchdog so a run proves the
scheduler and logger are alive. STM32F401RE uses the hardware watchdog instead.

o PlantSensorTask
Runs in the implicit App module. It generates a synthetic sensor sample every
500 ms and sends that sample by copied direct async message to
PlantControllerTask.

o PlantControllerTask
Runs in the same App module. It receives copied samples, locks the module-local
plantStateMutex, updates plantState, computes a command, unlocks the mutex and
posts plantActuatorSema.

o PlantActuatorTask
Runs in the same App module. It blocks on plantActuatorSema, reads the latest
command from plantState while holding plantStateMutex, then logs the command.
The semaphore is the scheduling event; the task does not poll.

o IsoAlphaTask
Is created with kTaskInitIsolated(), so it has a private one-task module. It
sends copied requests to IsoBetaTask and IsoGammaTask, then waits for copied
replies. No writable state crosses the module boundary.

o IsoBetaTask and IsoGammaTask
Each responder is also an isolated one-task module. It keeps private counters on
its own stack, receives copied requests and replies with copied payloads.

o FleetPlannerTask
Runs in fleetControlModule. It owns the desired fleet state and sends each order
to fleetCommsModule through a copied message queue.

o FleetLinkTxTask
Runs in fleetCommsModule. It receives copied orders, validates them and updates
module-local comms state under fleetCommsMutex.

o FleetSupervisorTask
Runs in fleetCommsModule. It is a synchronous call/reply server: it accepts a
copied status request, snapshots comms state under fleetCommsMutex and replies
with a copied FleetStatusReply.

o FleetTelemetryTask
Runs in fleetControlModule. It periodically makes a copied call/reply request to
FleetSupervisorTask and logs the copied reply.

***************************************************************************************/


#include <kapi.h>
#include <khal.h>
#include <klogger.h>
#include <kstring.h>

#define APP_LOG_PRIO (3U)

#if (K_HAL_HAS_WATCHDOG == 0U)
#define DEBUG_STACK_WORDS (256U)
#endif
#define PLANT_STACK_WORDS (256U)

#define ISO_STACK_WORDS (256U)
#define ISO_ALPHA_ID (1UL)
#define ISO_BETA_ID (2UL)
#define ISO_GAMMA_ID (3UL)
#define ISO_ALPHA_MAGIC (0xA10A10A1UL)
#define ISO_BETA_MAGIC (0xB20B20B2UL)
#define ISO_GAMMA_MAGIC (0xC30C30C3UL)

#define FLEET_MODULE_BYTES (4096U)
#define FLEET_STACK_WORDS (256U)
#define FLEET_ORDER_QUEUE_DEPTH (4U)

#if (K_HAL_HAS_WATCHDOG == 0U)
#define PRIO_DEBUG_HEARTBEAT (16U)
#endif
#define PRIO_PLANT_SENSOR (4U)
#define PRIO_PLANT_CONTROLLER (5U)
#define PRIO_PLANT_ACTUATOR (6U)
#define PRIO_ISO_ALPHA (7U)
#define PRIO_ISO_BETA (8U)
#define PRIO_ISO_GAMMA (9U)
#define PRIO_FLEET_LINK (10U)
#define PRIO_FLEET_SUPERVISOR (11U)
#define PRIO_FLEET_PLANNER (12U)
#define PRIO_FLEET_TELEMETRY (13U)

typedef struct
{
    ULONG seq;
    ULONG raw;
    ULONG filtered;
    ULONG setpoint;
} PlantSample;

typedef struct
{
    ULONG sampleSeq;
    ULONG raw;
    ULONG filtered;
    ULONG setpoint;
    ULONG command;
    ULONG commandSeq;
} PlantState;

typedef struct
{
    ULONG magic;
    ULONG rxCount;
    ULONG txCount;
    ULONG lastPeer;
} IsoState;

typedef struct
{
    ULONG source;
    ULONG seq;
    ULONG value;
    ULONG observed;
} IsoCopyMsg;

typedef struct
{
    ULONG planSeq;
    ULONG demand;
    ULONG mode;
    ULONG telemetryReads;
} FleetControlState;

typedef struct
{
    ULONG lastOrderSeq;
    ULONG linkBeat;
    ULONG supervisorCalls;
    ULONG lastDemand;
    ULONG lastMode;
} FleetCommsState;

typedef struct
{
    ULONG seq;
    ULONG mode;
    ULONG demand;
    ULONG checksum;
} FleetOrder;

typedef struct
{
    ULONG seq;
    ULONG expectedOrderSeq;
} FleetStatusReq;

typedef struct
{
    ULONG seq;
    ULONG lastOrderSeq;
    ULONG linkBeat;
    ULONG demand;
    ULONG status;
} FleetStatusReply;

/* APP MODULE: default kTaskInit() tasks share the implicit App module. */
#if (K_HAL_HAS_WATCHDOG == 0U)
RK_DECLARE_TASK(debugHeartbeatHandle, DebugHeartbeatTask, debugHeartbeatStack,
                DEBUG_STACK_WORDS)
#endif
RK_DECLARE_TASK(plantSensorHandle, PlantSensorTask, plantSensorStack,
                PLANT_STACK_WORDS)
RK_DECLARE_TASK(plantControllerHandle, PlantControllerTask, plantControllerStack,
                PLANT_STACK_WORDS)
RK_DECLARE_TASK(plantActuatorHandle, PlantActuatorTask, plantActuatorStack,
                PLANT_STACK_WORDS)
static PlantState plantState;

static RK_DECLARE_MUTEX(plantStateMutex)
static RK_DECLARE_SEMAPHORE(plantActuatorSema)

/* FLEET: two explicit multi-task modules backed by raw byte MPU windows. */
RK_DECLARE_MODULE(fleetControlModule, fleetControlRam, FLEET_MODULE_BYTES)
RK_DECLARE_MODULE(fleetCommsModule, fleetCommsRam, FLEET_MODULE_BYTES)
RK_DECLARE_MODULE_TASK(fleetPlannerHandle, FleetPlannerTask)
RK_DECLARE_MODULE_TASK(fleetTelemetryHandle, FleetTelemetryTask)
RK_DECLARE_MODULE_TASK(fleetLinkTxHandle, FleetLinkTxTask)
RK_DECLARE_MODULE_TASK(fleetSupervisorHandle, FleetSupervisorTask)

static FleetControlState *fleetControlState;
static FleetCommsState *fleetCommsState;

static RK_DECLARE_MUTEX(fleetCommsMutex)
static RK_DECLARE_MESG_QUEUE_HANDLE(fleetOrderQueueHandle)
static RK_DECLARE_MESG_QUEUE_BUF(fleetOrderQueueBuf, FleetOrder,
                                 FLEET_ORDER_QUEUE_DEPTH)

/* ISO: each task opts into a private one-task module. */
RK_DECLARE_ISOLATED_TASK(isoAlphaHandle, IsoAlphaTask, isoAlphaStack,
                         ISO_STACK_WORDS)
RK_DECLARE_ISOLATED_TASK(isoBetaHandle, IsoBetaTask, isoBetaStack,
                         ISO_STACK_WORDS)
RK_DECLARE_ISOLATED_TASK(isoGammaHandle, IsoGammaTask, isoGammaStack,
                         ISO_STACK_WORDS)

#if (K_HAL_HAS_WATCHDOG == 1U)
RK_DECLARE_TIMER(appWatchdogTimer)
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

#if (K_HAL_HAS_WATCHDOG == 1U)
static VOID AppWatchdogTimer_(VOID *args)
{
    RK_UNUSEARGS

    kHalWatchdogKick();
}
#endif

static VOID AppConfigureWatchdog_(VOID)
{
#if (K_HAL_HAS_WATCHDOG == 1U)
    kHalWatchdogConfigure();
    AppCheck_(kTimerCreate(&appWatchdogTimer, "WdgTmr", 0UL,
                           RK_MS_TO_TICKS(K_HAL_WATCHDOG_FEED_MS),
                           AppWatchdogTimer_, RK_NO_ARGS, RK_TIMER_RELOAD));
#endif
}

static VOID AppCreateModules_(VOID)
{
    RK_MEMSET(&plantState, 0, sizeof(plantState));
    RK_MEMSET(fleetControlRam, 0, sizeof(fleetControlRam));
    RK_MEMSET(fleetCommsRam, 0, sizeof(fleetCommsRam));

    AppCheck_(kModuleInit(&fleetControlModule,
                          fleetControlRam, sizeof(fleetControlRam), "FleetC"));
    AppCheck_(kModuleInit(&fleetCommsModule,
                          fleetCommsRam, sizeof(fleetCommsRam), "FleetM"));

    fleetControlState =
        AppCheckPtr_(RK_MODULE_ALLOC(&fleetControlModule, FleetControlState));

    fleetCommsState =
        AppCheckPtr_(RK_MODULE_ALLOC(&fleetCommsModule, FleetCommsState));
}

static VOID AppCreateObjects_(VOID)
{
    AppCheck_(kMutexCreate(&plantStateMutex, "PlantM", RK_PRIO_INHERITANCE));
    AppCheck_(kSemaphoreCreate(&plantActuatorSema, "PlantS", 0U, 1U));

    AppCheck_(kMutexCreateModuleScope(&fleetCommsMutex, "FleetM",
                                      RK_PRIO_INHERITANCE, &fleetCommsModule));
    AppCheck_(kMesgQueueCreateGlobalScope(&fleetOrderQueueHandle, "FleetQ",
                                          fleetOrderQueueBuf,
                                          RK_MESGQ_MESG_SIZE(FleetOrder),
                                          FLEET_ORDER_QUEUE_DEPTH));
}

static VOID AppCreateTasks_(VOID)
{
#if (K_HAL_HAS_WATCHDOG == 0U)
    AppCheck_(kTaskInit(&debugHeartbeatHandle, DebugHeartbeatTask,
                        RK_NO_ARGS, "DbgBeat", debugHeartbeatStack,
                        DEBUG_STACK_WORDS, PRIO_DEBUG_HEARTBEAT,
                        RK_PREEMPT));
#endif

    AppCheck_(kTaskInit(&plantSensorHandle, PlantSensorTask,
                        RK_NO_ARGS, "PltSens",
                        plantSensorStack, PLANT_STACK_WORDS,
                        PRIO_PLANT_SENSOR, RK_PREEMPT));
    AppCheck_(kTaskInit(&plantControllerHandle, PlantControllerTask,
                        RK_NO_ARGS, "PltCtrl",
                        plantControllerStack, PLANT_STACK_WORDS,
                        PRIO_PLANT_CONTROLLER, RK_PREEMPT));
    AppCheck_(kTaskInit(&plantActuatorHandle, PlantActuatorTask,
                        RK_NO_ARGS, "PltAct",
                        plantActuatorStack, PLANT_STACK_WORDS,
                        PRIO_PLANT_ACTUATOR, RK_PREEMPT));
    AppCheck_(kMesgCopyEndpointInit(plantControllerHandle));

    AppCheck_(kTaskInitIsolated(&isoAlphaHandle, IsoAlphaTask, RK_NO_ARGS,
                                "IsoA", isoAlphaStack, ISO_STACK_WORDS,
                                PRIO_ISO_ALPHA, RK_PREEMPT));
    AppCheck_(kTaskInitIsolated(&isoBetaHandle, IsoBetaTask, RK_NO_ARGS,
                                "IsoB", isoBetaStack, ISO_STACK_WORDS,
                                PRIO_ISO_BETA, RK_PREEMPT));
    AppCheck_(kTaskInitIsolated(&isoGammaHandle, IsoGammaTask, RK_NO_ARGS,
                                "IsoC", isoGammaStack, ISO_STACK_WORDS,
                                PRIO_ISO_GAMMA, RK_PREEMPT));

    AppCheck_(kMesgCopyEndpointInit(isoAlphaHandle));
    AppCheck_(kMesgCopyEndpointInit(isoBetaHandle));
    AppCheck_(kMesgCopyEndpointInit(isoGammaHandle));

    AppCheck_(kModuleTaskInit(&fleetControlModule, &fleetPlannerHandle,
                              FleetPlannerTask, fleetControlState,
                              "FltPlan", FLEET_STACK_WORDS,
                              PRIO_FLEET_PLANNER, RK_PREEMPT));
    AppCheck_(kModuleTaskInit(&fleetControlModule, &fleetTelemetryHandle,
                              FleetTelemetryTask, fleetControlState,
                              "FltTel", FLEET_STACK_WORDS,
                              PRIO_FLEET_TELEMETRY, RK_PREEMPT));
    AppCheck_(kModuleTaskInit(&fleetCommsModule, &fleetLinkTxHandle,
                              FleetLinkTxTask, fleetCommsState,
                              "FltTx", FLEET_STACK_WORDS,
                              PRIO_FLEET_LINK, RK_PREEMPT));
    AppCheck_(kModuleTaskInit(&fleetCommsModule, &fleetSupervisorHandle,
                              FleetSupervisorTask, fleetCommsState,
                              "FltSup", FLEET_STACK_WORDS,
                              PRIO_FLEET_SUPERVISOR, RK_PREEMPT));

    AppCheck_(kSynchMesgInit(fleetSupervisorHandle,
                             sizeof(FleetStatusReq)));
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
    AppCreateModules_();
    AppCreateObjects_();
    kLogInit(APP_LOG_PRIO);
    AppConfigureWatchdog_();
    AppCreateTasks_();
#if (RK_CONF_SYSMON == ON)
    AppCheck_(kSysMonInit());
#endif
}

/******************************************************************************
 * DEBUG: simple scheduler/logger heartbeat
 ******************************************************************************/

#if (K_HAL_HAS_WATCHDOG == 0U)
VOID DebugHeartbeatTask(VOID *args)
{
    ULONG beat = 0UL;

    RK_UNUSEARGS

    kLog("DEBUG heartbeat ready: scheduler started and logger is draining");

    while (1)
    {
        beat++;
        kLog("DEBUG heartbeat beat=%lu", beat);
        kSleep(RK_MS_TO_TICKS(1000UL));
    }
}
#endif

/******************************************************************************
 * PLANT: one module with several tasks
 ******************************************************************************/

VOID PlantSensorTask(VOID *args)
{
    ULONG seq = 0UL;
    ULONG filtered = 0UL;

    RK_UNUSEARGS

    plantState.setpoint = 48UL;
    kLog("APP module ready: plant tasks share state through the implicit module");

    while (1)
    {
        PlantSample sample;

        seq++;
        sample.seq = seq;
        sample.raw = ((seq * 37UL) + 11UL) % 100UL;
        filtered = ((filtered * 3UL) + sample.raw) / 4UL;
        sample.filtered = filtered;
        sample.setpoint = plantState.setpoint;

        AppCheck_(kMesgSendCopy(plantControllerHandle, &sample,
                                sizeof(sample)));
        kLog("PLANT sensor sent sample seq=%lu raw=%lu filtered=%lu setpoint=%lu",
             sample.seq, sample.raw, sample.filtered, sample.setpoint);

        kSleep(RK_MS_TO_TICKS(500UL));
    }
}

VOID PlantControllerTask(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        PlantSample sample;
        ULONG rxBytes = 0UL;
        ULONG command;

        AppCheck_(kMesgRecvCopy(plantSensorHandle, &sample, sizeof(sample),
                                &rxBytes, RK_WAIT_FOREVER));
        K_ASSERT(rxBytes == sizeof(sample));

        AppCheck_(kMutexLock(plantStateMutex, RK_WAIT_FOREVER));
        plantState.sampleSeq = sample.seq;
        plantState.raw = sample.raw;
        plantState.filtered = sample.filtered;
        plantState.setpoint = sample.setpoint;
        command = (sample.filtered < sample.setpoint) ? 1UL : 0UL;
        plantState.command = command;
        plantState.commandSeq++;
        AppCheck_(kMutexUnlock(plantStateMutex));

        RK_ERR const err = kSemaphorePost(plantActuatorSema);
        K_ASSERT((err == RK_ERR_SUCCESS) || (err == RK_ERR_SEMA_FULL));

        kLog("PLANT controller accepted sample seq=%lu command=%lu",
             sample.seq, command);
    }
}

VOID PlantActuatorTask(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        ULONG seq;
        ULONG command;
        ULONG raw;

        AppCheck_(kSemaphorePend(plantActuatorSema, RK_WAIT_FOREVER));

        AppCheck_(kMutexLock(plantStateMutex, RK_WAIT_FOREVER));
        seq = plantState.commandSeq;
        command = plantState.command;
        raw = plantState.raw;
        AppCheck_(kMutexUnlock(plantStateMutex));

        kLog("PLANT actuator applied command seq=%lu command=%lu raw=%lu",
             seq, command, raw);
    }
}

/******************************************************************************
 * ISO: several isolated one-task modules
 ******************************************************************************/

VOID IsoAlphaTask(VOID *args)
{
    IsoState state;
    ULONG seq = 0UL;

    RK_UNUSEARGS

    RK_MEMSET(&state, 0, sizeof(state));
    state.magic = ISO_ALPHA_MAGIC;
    kLog("ISO isolated tasks ready: alpha exchanges copied messages");

    while (1)
    {
        IsoCopyMsg request;

        seq++;
        request.source = ISO_ALPHA_ID;
        request.seq = seq;
        request.value = state.magic;
        request.observed = state.rxCount;

        state.txCount += 2UL;
        AppCheck_(kMesgSendCopy(isoBetaHandle, &request, sizeof(request)));
        AppCheck_(kMesgSendCopy(isoGammaHandle, &request, sizeof(request)));

        for (UINT replies = 0U; replies < 2U; replies++)
        {
            IsoCopyMsg reply;
            ULONG rxBytes = 0UL;

            AppCheck_(kMesgRecvCopy(RK_ANY_TASK, &reply, sizeof(reply),
                                    &rxBytes, RK_WAIT_FOREVER));
            K_ASSERT(rxBytes == sizeof(reply));

            state.rxCount++;
            state.lastPeer = reply.source;
            kLog("ISO alpha received reply from node=%lu seq=%lu peerSeen=%lu",
                 reply.source, reply.seq, reply.observed);
        }

        kSleep(RK_MS_TO_TICKS(1000UL));
    }
}

static VOID IsoResponder_(ULONG const source, ULONG const magic)
{
    IsoState state;

    RK_MEMSET(&state, 0, sizeof(state));
    state.magic = magic;

    while (1)
    {
        IsoCopyMsg request;
        IsoCopyMsg reply;
        ULONG rxBytes = 0UL;

        AppCheck_(kMesgRecvCopy(RK_ANY_TASK, &request, sizeof(request),
                                &rxBytes, RK_WAIT_FOREVER));
        K_ASSERT(rxBytes == sizeof(request));

        state.rxCount++;
        state.lastPeer = request.source;
        state.txCount++;

        reply.source = source;
        reply.seq = request.seq;
        reply.value = state.magic;
        reply.observed = state.rxCount;

        kLog("ISO node=%lu received request from peer=%lu seq=%lu",
             source, request.source, request.seq);
        AppCheck_(kMesgSendCopy(isoAlphaHandle, &reply, sizeof(reply)));
    }
}

VOID IsoBetaTask(VOID *args)
{
    RK_UNUSEARGS
    IsoResponder_(ISO_BETA_ID, ISO_BETA_MAGIC);
}

VOID IsoGammaTask(VOID *args)
{
    RK_UNUSEARGS
    IsoResponder_(ISO_GAMMA_ID, ISO_GAMMA_MAGIC);
}

/******************************************************************************
 * FLEET: several modules with several tasks
 ******************************************************************************/

VOID FleetPlannerTask(VOID *args)
{
    ULONG seq = 0UL;
    FleetControlState *const controlPtr = (FleetControlState *)args;

    K_ASSERT(controlPtr != NULL);

    kLog("FLEET modules ready: planner queues orders to comms module");

    while (1)
    {
        FleetOrder order;

        seq++;
        order.seq = seq;
        order.mode = seq & 1UL;
        order.demand = 20UL + ((seq * 3UL) % 30UL);
        order.checksum = order.seq ^ order.mode ^ order.demand;

        controlPtr->planSeq = order.seq;
        controlPtr->mode = order.mode;
        controlPtr->demand = order.demand;

        AppCheck_(kMesgQueueSend(fleetOrderQueueHandle, &order,
                                 RK_WAIT_FOREVER));
        kLog("FLEET planner sent copied order seq=%lu mode=%lu demand=%lu",
             order.seq, order.mode, order.demand);

        kSleep(RK_MS_TO_TICKS(1000UL));
    }
}

VOID FleetLinkTxTask(VOID *args)
{
    FleetCommsState *const commsPtr = (FleetCommsState *)args;

    K_ASSERT(commsPtr != NULL);

    while (1)
    {
        FleetOrder order;
        ULONG beat;

        AppCheck_(kMesgQueueRecv(fleetOrderQueueHandle, &order,
                                 RK_WAIT_FOREVER));
        K_ASSERT(order.checksum == (order.seq ^ order.mode ^ order.demand));

        AppCheck_(kMutexLock(fleetCommsMutex, RK_WAIT_FOREVER));
        commsPtr->lastOrderSeq = order.seq;
        commsPtr->lastDemand = order.demand;
        commsPtr->lastMode = order.mode;
        commsPtr->linkBeat++;
        beat = commsPtr->linkBeat;
        AppCheck_(kMutexUnlock(fleetCommsMutex));

        kLog("FLEET link consumed copied order seq=%lu beat=%lu",
             order.seq, beat);
    }
}

VOID FleetSupervisorTask(VOID *args)
{
    FleetCommsState *const commsPtr = (FleetCommsState *)args;

    K_ASSERT(commsPtr != NULL);

    while (1)
    {
        RK_SYNCH_CALL_DATA call;
        FleetStatusReq req;
        FleetStatusReply reply;
        ULONG reqBytes = 0UL;

        AppCheck_(kSynchMesgAccept(&call, &req, &reqBytes,
                                   RK_WAIT_FOREVER));
        K_ASSERT(reqBytes == sizeof(req));

        AppCheck_(kMutexLock(fleetCommsMutex, RK_WAIT_FOREVER));
        commsPtr->supervisorCalls++;
        reply.seq = req.seq;
        reply.lastOrderSeq = commsPtr->lastOrderSeq;
        reply.linkBeat = commsPtr->linkBeat;
        reply.demand = commsPtr->lastDemand;
        reply.status = ((req.expectedOrderSeq != 0UL) &&
                        (commsPtr->lastOrderSeq == req.expectedOrderSeq))
                           ? 1UL
                           : 0UL;
        AppCheck_(kMutexUnlock(fleetCommsMutex));

        AppCheck_(kSynchMesgReply(&call, &reply, sizeof(reply)));

        kLog("FLEET supervisor replied status req=%lu lastOrder=%lu beat=%lu inSync=%lu",
             reply.seq, reply.lastOrderSeq, reply.linkBeat, reply.status);
    }
}

VOID FleetTelemetryTask(VOID *args)
{
    ULONG seq = 0UL;
    FleetControlState *const controlPtr = (FleetControlState *)args;

    K_ASSERT(controlPtr != NULL);

    while (1)
    {
        FleetStatusReq req;
        FleetStatusReply reply;
        RK_SYNCH_ATTR attr;
        ULONG replyBytes = 0UL;

        seq++;
        req.seq = seq;
        req.expectedOrderSeq = controlPtr->planSeq;
        attr.reqPtr = &req;
        attr.reqBytes = sizeof(req);
        attr.replyPtr = &reply;
        attr.replyMaxBytes = sizeof(reply);
        attr.replyBytesPtr = &replyBytes;

        AppCheck_(kSynchMesgCall(fleetSupervisorHandle, &attr,
                                 RK_WAIT_FOREVER));
        K_ASSERT(replyBytes == sizeof(reply));

        controlPtr->telemetryReads++;

        kLog("FLEET telemetry received status req=%lu lastOrder=%lu beat=%lu inSync=%lu",
             reply.seq, reply.lastOrderSeq, reply.linkBeat, reply.status);

        kSleepRelease(RK_MS_TO_TICKS(1500UL));
    }
}
