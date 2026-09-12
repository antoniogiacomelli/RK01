/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Signal/synchronous-call regression:
 *   - a masked pending signal must not interrupt a blocked syscall;
 *   - a signal interrupting a queued kSynchMesgCall() must remove the caller
 *     from the server queue and recompute server priority;
 *   - a signal interrupting an active kSynchMesgCall() must mark the call
 *     abandoned, recompute server priority, and let reply clear the server side.
 */

#include <kapi_app.h>

#if (RK_CONF_SYNCH_MESG != ON)
#error "signal_synch_wait_cleanup requires RK_CONF_SYNCH_MESG=ON"
#endif

#define REG_TASK_STACK_WORDS (512U)
#define REG_ALT_STACK_WORDS (RK_CONF_MIN_STACKSIZE)

#define REG_CONTROLLER_PRIO (15U)
#define REG_SERVER_PRIO (12U)
#define REG_CALLER_PRIO (7U)

#define REG_SIGNAL RK_SIGNAL_1
#define REG_SIGNAL_BIT ((RK_SIGNAL)1UL << (REG_SIGNAL - 1UL))

#define EV_CALLER_READY RK_EVENT_1
#define EV_SERVER_READY RK_EVENT_2
#define EV_MASK_WAITING RK_EVENT_3
#define EV_MASK_DONE RK_EVENT_4
#define EV_QUEUE_CALL_DONE RK_EVENT_5
#define EV_QUEUE_SERVER_DONE RK_EVENT_6
#define EV_SERVER_ACCEPT_WAITING RK_EVENT_7
#define EV_ACTIVE_CALL_DONE RK_EVENT_8
#define EV_ACTIVE_SERVER_DONE RK_EVENT_9

#define EV_CALLER_RELEASE RK_EVENT_10
#define EV_CALLER_QUEUE_CALL RK_EVENT_11
#define EV_CALLER_ACTIVE_CALL RK_EVENT_12
#define EV_SERVER_CHECK_QUEUE RK_EVENT_13
#define EV_SERVER_ACCEPT_ACTIVE RK_EVENT_14

#define REQ_QUEUED (0x51554555UL)
#define REQ_ACTIVE (0x41435456UL)
#define REPLY_OK (0x5245504CUL)

typedef struct SignalSynchReq
{
    ULONG op;
    ULONG value;
} SignalSynchReq;

typedef struct SignalSynchReply
{
    ULONG value;
} SignalSynchReply;

RK_DECLARE_TASK(controllerHandle, ControllerTask, controllerStack,
                REG_TASK_STACK_WORDS)
RK_DECLARE_TASK(serverHandle, ServerTask, serverStack, REG_TASK_STACK_WORDS)
RK_DECLARE_TASK(callerHandle, CallerTask, callerStack, REG_TASK_STACK_WORDS)
RK_DECLARE_TASK_HANDLE(overlapHandle)

static RK_STACK callerAltStack[REG_ALT_STACK_WORDS] K_ALIGN(8)
    RK_SECTION_APP_RAM;
static volatile ULONG signalCount RK_SHARED_RAM_ATTR;

static VOID SignalHandler_(RK_SIGNAL const signal)
{
    if (signal == REG_SIGNAL)
    {
        signalCount++;
    }
}

static VOID OverlapTask_(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        (VOID)kSleepDelay(RK_MS_TO_TICKS(1000UL));
    }
}

static VOID Expect_(RK_BOOL const ok, RK_FAULT const fault)
{
    K_ASSERT(ok == RK_TRUE);
    if (ok != RK_TRUE)
    {
        while (1)
        {
            kErrHandler(fault);
        }
    }
}

static VOID WaitEvent_(RK_TASK_EVENT const event)
{
    RK_ERR const err = kEventGet(event, RK_OPT_EVENT_ANY, NULL,
                                 RK_WAIT_FOREVER);
    Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
            (RK_FAULT)err);
}

static VOID SetEvent_(RK_TASK_HANDLE const handle, RK_TASK_EVENT const event)
{
    RK_ERR const err = kEventSet(handle, event);
    Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
            (RK_FAULT)err);
}

static RK_ERR CallServer_(ULONG const op)
{
    SignalSynchReq req = {.op = op, .value = op ^ 0x11111111UL};
    SignalSynchReply reply = {.value = 0UL};
    ULONG replyBytes = 0UL;
    RK_SYNCH_ATTR attr = {
        .reqPtr = &req,
        .reqBytes = sizeof(req),
        .replyPtr = &reply,
        .replyMaxBytes = sizeof(reply),
        .replyBytesPtr = &replyBytes,
    };

    return (kSynchMesgCall(serverHandle, &attr, RK_WAIT_FOREVER));
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
    kLogInit(REG_CONTROLLER_PRIO);

    {
        RK_ERR const err = kTaskInit(&controllerHandle, ControllerTask,
                                     RK_NO_ARGS, "Ctl", controllerStack,
                                     REG_TASK_STACK_WORDS,
                                     REG_CONTROLLER_PRIO, RK_PREEMPT);
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
    {
        RK_ERR const err = kTaskInit(&serverHandle, ServerTask, RK_NO_ARGS,
                                     "Srv", serverStack, REG_TASK_STACK_WORDS,
                                     REG_SERVER_PRIO, RK_PREEMPT);
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
    {
        RK_ERR const err = kTaskInit(&callerHandle, CallerTask, RK_NO_ARGS,
                                     "Cal", callerStack, REG_TASK_STACK_WORDS,
                                     REG_CALLER_PRIO, RK_PREEMPT);
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
    {
        RK_ERR const err = kSynchMesgInit(serverHandle,
                                          sizeof(SignalSynchReq));
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
}

VOID CallerTask(VOID *args)
{
    RK_UNUSEARGS

    {
        RK_ERR const err = kSignalHandlerSet(REG_SIGNAL, SignalHandler_,
                                             callerStack,
                                             sizeof(callerStack));
        Expect_((err == RK_ERR_INVALID_PARAM) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
    {
        RK_ERR const err = kSignalHandlerSet(REG_SIGNAL, SignalHandler_,
                                             callerAltStack,
                                             sizeof(callerAltStack));
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
    {
        RK_ERR const err = kTaskInit(&overlapHandle, OverlapTask_,
                                     RK_NO_ARGS, "Ovl", callerAltStack,
                                     REG_ALT_STACK_WORDS, REG_CALLER_PRIO,
                                     RK_PREEMPT);
        Expect_((err == RK_ERR_INVALID_PARAM) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }

    SetEvent_(controllerHandle, EV_CALLER_READY);

    {
        ULONG const before = signalCount;
        RK_ERR err = kSignalMaskSet(0UL);
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);

        SetEvent_(controllerHandle, EV_MASK_WAITING);
        err = kEventGet(EV_CALLER_RELEASE, RK_OPT_EVENT_ANY, NULL,
                        RK_WAIT_FOREVER);
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
        Expect_((signalCount == before) ? RK_TRUE : RK_FALSE,
                RK_FAULT_APP_CRASH);

        err = kSignalMaskSet(REG_SIGNAL_BIT);
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
        Expect_((signalCount == (before + 1UL)) ? RK_TRUE : RK_FALSE,
                RK_FAULT_APP_CRASH);
    }
    SetEvent_(controllerHandle, EV_MASK_DONE);

    WaitEvent_(EV_CALLER_QUEUE_CALL);
    {
        RK_ERR const err = CallServer_(REQ_QUEUED);
        Expect_((err == RK_ERR_SIGNAL_INTERRUPTED) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
    SetEvent_(controllerHandle, EV_QUEUE_CALL_DONE);

    WaitEvent_(EV_CALLER_ACTIVE_CALL);
    {
        RK_ERR const err = CallServer_(REQ_ACTIVE);
        Expect_((err == RK_ERR_SIGNAL_INTERRUPTED) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
    SetEvent_(controllerHandle, EV_ACTIVE_CALL_DONE);

    while (1)
    {
        (VOID)kSleepDelay(RK_MS_TO_TICKS(1000UL));
    }
}

VOID ServerTask(VOID *args)
{
    RK_UNUSEARGS

    SetEvent_(controllerHandle, EV_SERVER_READY);

    WaitEvent_(EV_SERVER_CHECK_QUEUE);
    {
        RK_SYNCH_CALL_DATA call;
        SignalSynchReq req;
        ULONG reqBytes = 0UL;
        RK_ERR const err = kSynchMesgAccept(&call, &req, &reqBytes,
                                            RK_NO_WAIT);
        Expect_((err == RK_ERR_BUFFER_EMPTY) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
        Expect_((kTaskGetPrio(serverHandle) == REG_SERVER_PRIO) ? RK_TRUE :
                                                               RK_FALSE,
                RK_FAULT_APP_CRASH);
    }
    SetEvent_(controllerHandle, EV_QUEUE_SERVER_DONE);

    WaitEvent_(EV_SERVER_ACCEPT_ACTIVE);
    SetEvent_(controllerHandle, EV_SERVER_ACCEPT_WAITING);
    {
        RK_SYNCH_CALL_DATA call;
        SignalSynchReq req;
        ULONG reqBytes = 0UL;
        RK_ERR err = kSynchMesgAccept(&call, &req, &reqBytes,
                                      RK_WAIT_FOREVER);
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
        Expect_(((req.op == REQ_ACTIVE) && (reqBytes == sizeof(req))) ?
                    RK_TRUE :
                    RK_FALSE,
                RK_FAULT_APP_CRASH);

        err = kSignalSend(call.caller, REG_SIGNAL);
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
        Expect_((kTaskGetPrio(serverHandle) == REG_SERVER_PRIO) ? RK_TRUE :
                                                               RK_FALSE,
                RK_FAULT_APP_CRASH);

        SignalSynchReply reply = {.value = REPLY_OK};
        err = kSynchMesgReply(&call, &reply, sizeof(reply));
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
    SetEvent_(controllerHandle, EV_ACTIVE_SERVER_DONE);

    while (1)
    {
        (VOID)kSleepDelay(RK_MS_TO_TICKS(1000UL));
    }
}

VOID ControllerTask(VOID *args)
{
    RK_UNUSEARGS

    WaitEvent_(EV_CALLER_READY);
    WaitEvent_(EV_SERVER_READY);

    WaitEvent_(EV_MASK_WAITING);
    {
        RK_ERR const err = kSignalSend(callerHandle, REG_SIGNAL);
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
    SetEvent_(callerHandle, EV_CALLER_RELEASE);
    WaitEvent_(EV_MASK_DONE);

    SetEvent_(callerHandle, EV_CALLER_QUEUE_CALL);
    Expect_((kTaskGetPrio(serverHandle) == REG_CALLER_PRIO) ? RK_TRUE :
                                                           RK_FALSE,
            RK_FAULT_APP_CRASH);
    {
        RK_ERR const err = kSignalSend(callerHandle, REG_SIGNAL);
        Expect_((err == RK_ERR_SUCCESS) ? RK_TRUE : RK_FALSE,
                (RK_FAULT)err);
    }
    WaitEvent_(EV_QUEUE_CALL_DONE);
    Expect_((kTaskGetPrio(serverHandle) == REG_SERVER_PRIO) ? RK_TRUE :
                                                           RK_FALSE,
            RK_FAULT_APP_CRASH);
    SetEvent_(serverHandle, EV_SERVER_CHECK_QUEUE);
    WaitEvent_(EV_QUEUE_SERVER_DONE);

    SetEvent_(serverHandle, EV_SERVER_ACCEPT_ACTIVE);
    WaitEvent_(EV_SERVER_ACCEPT_WAITING);
    SetEvent_(callerHandle, EV_CALLER_ACTIVE_CALL);
    WaitEvent_(EV_ACTIVE_CALL_DONE);
    WaitEvent_(EV_ACTIVE_SERVER_DONE);

    kLog("signal synchronous wait cleanup regression passed signals=%lu",
         signalCount);

    while (1)
    {
        (VOID)kSleepDelay(RK_MS_TO_TICKS(1000UL));
    }
}
