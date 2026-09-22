/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * File intent:
 *   RK0-compatible self-suspend/resume primitives adapted to RK01 task handles
 *   and SVC continuation semantics.
 */

#define RK_SOURCE_CODE

#include <ktasksuspend.h>
#include <ksch.h>
#include <ksyscall.h>

#if (RK_CONF_ERR_CHECK == ON)
static RK_BOOL kTaskSuspendIrqsDisabled_(VOID)
{
    unsigned state;

    RK_ASM volatile("MRS %0, PRIMASK" : "=r"(state) : : "memory");

    return ((state != 0U) ? RK_TRUE : RK_FALSE);
}
#endif

static RK_ERR kTaskSuspendReportErr_(RK_ERR const err)
{
#if (RK_CONF_ERR_CHECK == ON)
    if (err == RK_ERR_OBJ_NULL)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NULL);
    }
    else if (err == RK_ERR_OBJ_NOT_INIT)
    {
        K_ERR_HANDLER(RK_FAULT_OBJ_NOT_INIT);
    }
    else if (err == RK_ERR_INVALID_OBJ)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_OBJ);
    }
    else if (err == RK_ERR_INVALID_PARAM)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
    }
    else if (err == RK_ERR_INVALID_ISR_PRIMITIVE)
    {
        K_ERR_HANDLER(RK_FAULT_INVALID_ISR_PRIMITIVE);
    }
    else if (err == RK_ERR_TASK_INVALID_ST)
    {
        K_ERR_HANDLER(RK_FAULT_TASK_INVALID_STATE);
    }
    else
    {
        K_ERR_HANDLER((RK_FAULT)err);
    }
#else
    K_UNUSE(err);
#endif
    return (err);
}

static RK_ERR kTaskSelfSuspendPrecheck_(VOID)
{
    if (kIsISR() != 0U)
    {
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if ((RK_gRunPtr == NULL) || (RK_gRunPtr->status != RK_RUNNING))
    {
        return (RK_ERR_TASK_INVALID_ST);
    }

#if (RK_CONF_ERR_CHECK == ON)
    if ((RK_gSchLock != 0U) ||
        (kTaskSuspendIrqsDisabled_() == RK_TRUE))
#else
    if (RK_gSchLock != 0U)
#endif
    {
        return (RK_ERR_TASK_INVALID_ST);
    }

    return (RK_ERR_SUCCESS);
}

static RK_ERR
kTaskSelfSuspendEnter_(RK_EXCEPTION_FRAME *const framePtr,
                       RK_BOOL const syscall)
{
    RK_ERR err = kTaskSelfSuspendPrecheck_();
    if (err != RK_ERR_SUCCESS)
    {
        return (kTaskSuspendReportErr_(err));
    }

    RK_CR_AREA
    RK_CR_ENTER

    if ((RK_gRunPtr == NULL) || (RK_gRunPtr->status != RK_RUNNING) ||
        (RK_gSchLock != 0U))
    {
        RK_CR_EXIT
        return (kTaskSuspendReportErr_(RK_ERR_TASK_INVALID_ST));
    }

    RK_gRunPtr->status = RK_SELF_SUSPENDED;
    if (syscall == RK_TRUE)
    {
        kSyscallTaskSuspend(framePtr, RK_SYSCALL_TASK_SELF_SUSPEND,
                            0UL, 0UL, 0UL, 0UL,
                            RK_SYSCALL_PHASE_WAIT);
    }
    kPendCtxSwtch();

    RK_CR_EXIT

    return ((syscall == RK_TRUE) ? RK_ERR_SYSCALL_RESTART : RK_ERR_SUCCESS);
}

RK_ERR kTaskSelfSuspend(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TASK_SELF_SUSPEND,
                                        0UL, 0UL, 0UL, 0UL));
    }

    return (kTaskSelfSuspendEnter_(NULL, RK_FALSE));
}

RK_ERR kTaskSelfSuspendSyscall(RK_EXCEPTION_FRAME *const framePtr)
{
    return (kTaskSelfSuspendEnter_(framePtr, RK_TRUE));
}

RK_ERR kTaskResume(RK_TASK_HANDLE const taskHandle)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_TASK_RESUME,
                                        (ULONG)(UINTPTR)taskHandle,
                                        0UL, 0UL, 0UL));
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_TCB *taskPtr = NULL;
    RK_ERR err = kTaskHandleResolve(taskHandle, &taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_EXIT
        return (kTaskSuspendReportErr_(err));
    }

    if ((taskPtr == RK_gRunPtr) && (kIsISR() == 0U))
    {
        RK_CR_EXIT
        return (kTaskSuspendReportErr_(RK_ERR_INVALID_PARAM));
    }

    if (taskPtr->status != RK_SELF_SUSPENDED)
    {
        RK_CR_EXIT
        return (kTaskSuspendReportErr_(RK_ERR_TASK_INVALID_ST));
    }

    err = kReadySwtch(taskPtr);
    RK_CR_EXIT

    if ((err == RK_ERR_RESCHED_NOT_NEEDED) ||
        (err == RK_ERR_RESCHED_PENDING))
    {
        return (RK_ERR_SUCCESS);
    }

    return (err);
}
