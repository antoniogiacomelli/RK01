/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * File intent:
 *   Per-task event flags. Events are notification bits stored in the receiver
 *   task, with wait modes for ANY/ALL and optional timeout continuation.
 *
 * Contracts/invariants:
 *   - flagsCurr is the receiver's latched event mask.
 *   - flagsReq/flagsOpt are meaningful only while the task is waiting.
 *   - Successful waits consume only requested bits.
 *   - kEventSet ORs bits; it never clears unrelated notifications.
 */

#define RK_SOURCE_CODE

#include <ktaskevents.h>
#include <ksyscall.h>

/*****************************************************************************/
/* TASK EVENTS                                                               */
/*****************************************************************************/
/*
 * Event operations treat the requested event set as a mask:
 * - ALL waits require every requested bit to be present.
 * - ANY waits require at least one requested bit to be present.
 * On a successful wait, only the requested bits are consumed; unrelated event
 * bits stay latched on the task.
 */
RK_ERR kEventGet(ULONG const requiredFlags, UINT const getOptions,
                 ULONG *const gotFlagsPtr, RK_TICK const timeout)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_EVENT_GET, (ULONG)requiredFlags, (ULONG)getOptions,
            (ULONG)(UINTPTR)gotFlagsPtr, (ULONG)timeout));
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)

    /* check for invalid parameters and return specific error */
    /* an ISR has no task control block */
    if (kIsISR())
    {
        RK_CR_EXIT
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }
    /* check for invalid getOptions, including requiredFlags flags == 0 */
    if ((getOptions != RK_OPT_EVENT_ALL && getOptions != RK_OPT_EVENT_ANY) ||
        requiredFlags == 0UL)
    {
        RK_CR_EXIT
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        return (RK_ERR_INVALID_PARAM);
    }

#endif

    RK_gRunPtr->flagsReq = requiredFlags;
    RK_gRunPtr->flagsOpt = getOptions;

    /* inspecting the flags upon returning is optional */
    if (gotFlagsPtr != NULL)
        *gotFlagsPtr = RK_gRunPtr->flagsCurr;

    UINT andLogic = (getOptions == RK_OPT_EVENT_ALL);
    UINT conditionMet = 0;

    /* check if ANY or ALL flags establish a waiting condition */
    if (andLogic) /* ALL */
    {
        conditionMet =
            ((RK_gRunPtr->flagsCurr & requiredFlags) == (RK_gRunPtr->flagsReq));
    }
    else
    {
        conditionMet = (RK_gRunPtr->flagsCurr & requiredFlags);
    }

    /* if condition is met, clear flags and return */
    if (conditionMet)
    {
        RK_gRunPtr->flagsCurr &= ~RK_gRunPtr->flagsReq;
        RK_gRunPtr->flagsReq = 0UL;
        RK_gRunPtr->flagsOpt = 0UL;
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }
    /* condition not met, and non-blocking call, return FLAGS_NOT_MET */
    if (timeout == RK_NO_WAIT)
    {
        RK_CR_EXIT
        return (RK_ERR_FLAGS_NOT_MET);
    }

    /* Start suspension with flagsReq/flagsOpt describing the wake condition. */

    /* if bounded timeout, enqueue task on timeout list with no
        associated waiting queue */
    if ((timeout != RK_WAIT_FOREVER) && (timeout > 0))
    {
        RK_TASK_TIMEOUT_EVENTFLAGS

        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            RK_gRunPtr->timeoutNode.timeoutType = 0;
            RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
            RK_CR_EXIT
            return (err);
        }
    }
    RK_gRunPtr->status = RK_SLEEPING_EV_FLAG;
    /* Resume after timeout or after kEventSet() satisfies the mask. */
    kPendCtxSwtch();

    RK_CR_EXIT

    /* suspension is resumed here */
    RK_CR_ENTER
    /* if resuming reason is timeout return ERR_TIMEOUT */
    if (RK_gRunPtr->timeOut)
    {
        RK_gRunPtr->timeOut = RK_FALSE;
        RK_CR_EXIT
        return (RK_ERR_TIMEOUT);
    }

    /* resuming reason is a Set with condition met */

    /* if bounded waiting, remove task from timeout list */
    if ((timeout != RK_WAIT_FOREVER) && (timeout > 0) &&
        (RK_gRunPtr->timeoutNode.timeoutType == RK_TIMEOUT_EVENTFLAGS))
    {
        kRemoveTimeoutNode(&RK_gRunPtr->timeoutNode);
        RK_gRunPtr->timeoutNode.timeoutType = 0;
        RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
    }

    /* store current flags if asked */
    if (gotFlagsPtr != NULL)
        *gotFlagsPtr = RK_gRunPtr->flagsCurr;

    /* clear flags on the TCB and return SUCCESS */
    RK_gRunPtr->flagsCurr &= ~RK_gRunPtr->flagsReq;
    RK_gRunPtr->flagsReq = 0UL;
    RK_gRunPtr->flagsOpt = 0UL;
    RK_CR_EXIT

    return (RK_ERR_SUCCESS);
}

RK_ERR kEventGetSyscall(RK_EXCEPTION_FRAME *const framePtr,
                        RK_TASK_EVENT const requiredFlags,
                        RK_OPTION const getOptions,
                        RK_TASK_EVENT *const gotFlagsPtr,
                        RK_TICK const timeout)
{
    RK_CR_AREA
    RK_CR_ENTER

    if (((getOptions != RK_OPT_EVENT_ALL) &&
         (getOptions != RK_OPT_EVENT_ANY)) ||
        (requiredFlags == 0UL))
    {
        RK_CR_EXIT
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_INVALID_PARAM);
    }

    RK_gRunPtr->flagsReq = requiredFlags;
    RK_gRunPtr->flagsOpt = getOptions;

    if (gotFlagsPtr != NULL)
    {
        *gotFlagsPtr = RK_gRunPtr->flagsCurr;
    }

    UINT const andLogic = (getOptions == RK_OPT_EVENT_ALL);
    UINT const conditionMet =
        (andLogic != 0U)
            ? ((RK_gRunPtr->flagsCurr & requiredFlags) ==
               RK_gRunPtr->flagsReq)
            : (RK_gRunPtr->flagsCurr & requiredFlags);

    if (conditionMet != 0U)
    {
        RK_gRunPtr->flagsCurr &= ~RK_gRunPtr->flagsReq;
        RK_gRunPtr->flagsReq = 0UL;
        RK_gRunPtr->flagsOpt = 0UL;
        kSyscallTaskClear(RK_gRunPtr);
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    if (timeout == RK_NO_WAIT)
    {
        kSyscallTaskClear(RK_gRunPtr);
        RK_CR_EXIT
        return (RK_ERR_FLAGS_NOT_MET);
    }

    if ((timeout != RK_WAIT_FOREVER) && (timeout > 0UL))
    {
        RK_TASK_TIMEOUT_EVENTFLAGS

        RK_ERR err = kTimeoutNodeAdd(&RK_gRunPtr->timeoutNode, timeout);
        if (err != RK_ERR_SUCCESS)
        {
            RK_gRunPtr->timeoutNode.timeoutType = 0U;
            RK_gRunPtr->timeoutNode.waitingQueuePtr = NULL;
            kSyscallTaskClear(RK_gRunPtr);
            RK_CR_EXIT
            return (err);
        }
    }

    RK_gRunPtr->status = RK_SLEEPING_EV_FLAG;
    kSyscallTaskSuspend(framePtr, RK_SYSCALL_EVENT_GET,
                        (ULONG)requiredFlags, (ULONG)getOptions,
                        (ULONG)(UINTPTR)gotFlagsPtr, (ULONG)timeout,
                        RK_SYSCALL_PHASE_WAIT);
    kPendCtxSwtch();
    RK_CR_EXIT
    return (RK_ERR_SYSCALL_RESTART);
}

RK_ERR kEventSet(RK_TASK_HANDLE const receiverHandle, ULONG const setFlags)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_EVENT_SET, (ULONG)(UINTPTR)receiverHandle,
            (ULONG)setFlags, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)

    /* check for invalid parameters and return specific error */
    if (receiverHandle == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }
    if (setFlags == 0UL)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        RK_CR_EXIT
        return (RK_ERR_INVALID_PARAM);
    }

#endif

    RK_TCB *receiverPtr = NULL;
    RK_ERR resolveErr = kTaskHandleResolve(receiverHandle, &receiverPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER((RK_FAULT)resolveErr);
#endif
        RK_CR_EXIT
        return (resolveErr);
    }

    /* OR mask to current flags */
    receiverPtr->flagsCurr |= setFlags;
    if ((receiverPtr->status == RK_SLEEPING_EV_FLAG))
    {
        UINT andLogic = 0;
        UINT conditionMet = 0;

        andLogic = (receiverPtr->flagsOpt == RK_OPT_EVENT_ALL);

        if (andLogic)
        {
            conditionMet = ((receiverPtr->flagsCurr & receiverPtr->flagsReq) ==
                            (receiverPtr->flagsReq));
        }
        else
        {
            conditionMet = (receiverPtr->flagsCurr & receiverPtr->flagsReq);
        }

        /* if condition is met and task is pending, ready task
        and return SUCCESS */
        if (conditionMet)
        {
            if (receiverPtr->timeoutNode.timeoutType == RK_TIMEOUT_EVENTFLAGS)
            {
                kRemoveTimeoutNode(&receiverPtr->timeoutNode);
                receiverPtr->timeoutNode.timeoutType = 0;
                receiverPtr->timeoutNode.waitingQueuePtr = NULL;
            }
            kReadySwtch(receiverPtr);
        }
    }


    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}


RK_ERR kEventClear(RK_TASK_HANDLE taskHandle, ULONG const flagsToClear)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_EVENT_CLEAR, (ULONG)(UINTPTR)taskHandle,
            (ULONG)flagsToClear, 0UL, 0UL));
    }

    /* a clear cannot be interrupted */
    RK_CR_AREA
    RK_CR_ENTER

#if (RK_CONF_ERR_CHECK == ON)

    /* an ISR has no TCB */
    if (kIsISR() && (taskHandle == NULL))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }
    if (flagsToClear == 0UL)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
        RK_CR_EXIT
        return (RK_FAULT_INVALID_PARAM);
    }
#endif

    RK_TCB *taskPtr = NULL;
    RK_ERR resolveErr = kTaskHandleResolveOrRunning(taskHandle, &taskPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (resolveErr);
    }

    taskPtr->flagsCurr &= ~flagsToClear;
    RK_DMB
    RK_CR_EXIT

    return (RK_ERR_SUCCESS);
}

RK_ERR kEventQuery(RK_TASK_HANDLE const taskHandle, ULONG *const queryFlagsPtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_EVENT_QUERY, (ULONG)(UINTPTR)taskHandle,
            (ULONG)(UINTPTR)queryFlagsPtr, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER
#if (RK_CONF_ERR_CHECK == ON)
    if (queryFlagsPtr == NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
        RK_CR_EXIT
        return (RK_ERR_OBJ_NULL);
    }
    if (kIsISR() && (taskHandle == NULL))
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
        RK_CR_EXIT
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }
#endif
    RK_TCB *taskPtr = NULL;
    RK_ERR resolveErr = kTaskHandleResolveOrRunning(taskHandle, &taskPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (resolveErr);
    }

    (*queryFlagsPtr = taskPtr->flagsCurr);
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}
