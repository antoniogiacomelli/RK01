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
 *   Per-task software signals. A signal is pending state owned by the target
 *   TCB; delivery is a controlled user-context redirect at SVC return.
 *
 * Delivery model:
 *   kSignalSend() only sets a pending bit and makes the target runnable. At a
 *   kernel-to-user return boundary, kSignalMaybeDeliverOnReturn() selects the
 *   first enabled pending signal with a registered handler and builds a fake
 *   Cortex-M exception frame on the task's alternate signal stack. Exception
 *   return then resumes into kSignalTrampoline_(), which calls the user handler
 *   and uses kSignalReturn() to restore the task's original PSP.
 *
 *   The current frame builder supports only basic, non-FPU exception frames.
 *   FPU builds keep the rest of the kernel usable by rejecting signal-handler
 *   registration until the architecture ports save/restore the raw interrupted
 *   PSP and EXC_RETURN for extended FP frames.
 */

#define RK_SOURCE_CODE

#include <ksignal.h>
#include <kcoredefs.h>
#include <kmem.h>
#include <ksch.h>
#include <ksyscall.h>

#define RK_SIGNAL_XPSR_THUMB (0x01000000UL)

typedef VOID (*RK_SIGNAL_TRAMPOLINE_ENTRY)(RK_SIGNAL signal,
                                           RK_SIGNAL_HANDLER handler);
typedef RK_ERR (*RK_SIGNAL_RETURN_ENTRY)(VOID);

static VOID kSignalTrampoline_(RK_SIGNAL const signal,
                               RK_SIGNAL_HANDLER const handler)
    __attribute__((noreturn));

/*
 * Function pointers move through syscall ULONG slots and exception-frame
 * registers. Keep the conversions in one place so the rest of the code can
 * keep type intent visible.
 */
static VOID const *kSignalHandlerPtr_(RK_SIGNAL_HANDLER const handler)
{
    union
    {
        RK_SIGNAL_HANDLER handler;
        VOID const *ptr;
    } conv;

    conv.handler = handler;
    return (conv.ptr);
}

static VOID const *kSignalTrampolinePtr_(
    RK_SIGNAL_TRAMPOLINE_ENTRY const trampoline)
{
    union
    {
        RK_SIGNAL_TRAMPOLINE_ENTRY trampoline;
        VOID const *ptr;
    } conv;

    conv.trampoline = trampoline;
    return (conv.ptr);
}

static ULONG kSignalTrampolineThumbAddr_(VOID)
{
    return (((ULONG)(UINTPTR)kSignalTrampolinePtr_(kSignalTrampoline_)) | 1UL);
}

static VOID kSignalTrampoline_(RK_SIGNAL const signal,
                               RK_SIGNAL_HANDLER const handler)
{
    /*
     * The synthetic exception frame enters here rather than entering the user
     * handler directly. That gives every handler a single exit path back to the
     * interrupted task context, even when the handler simply returns.
     */
    if (handler != NULL)
    {
        handler(signal);
    }

    (VOID)kSignalReturn();
    K_ERR_HANDLER(RK_FAULT_INVALID_PHASE);

    while (1)
    {
    }
}

static ULONG kSignalHandlerArg_(RK_SIGNAL_HANDLER const handler)
{
    return ((ULONG)(UINTPTR)kSignalHandlerPtr_(handler));
}

static ULONG kSignalReturnThumbAddr_(VOID)
{
    union
    {
        RK_SIGNAL_RETURN_ENTRY returnEntry;
        VOID const *ptr;
    } conv;

    conv.returnEntry = kSignalReturn;
    return (((ULONG)(UINTPTR)conv.ptr) | 1UL);
}

static RK_BOOL kSignalExceptionFrameSupported_(VOID)
{
#if (RK_CONF_FPU == ON)
    return (RK_FALSE);
#else
    return (RK_TRUE);
#endif
}

static RK_ERR kSignalReturnKernel_(VOID **const savedPspPtr)
{
    RK_TCB *taskPtr;
    RK_CR_AREA

    if (savedPspPtr == NULL)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    *savedPspPtr = NULL;
    taskPtr = RK_gRunPtr;

    if ((taskPtr == NULL) || (taskPtr->signalActive != RK_TRUE) ||
        (taskPtr->signalSavedPsp == NULL))
    {
        return (RK_ERR_INVALID_PHASE);
    }

    /*
     * kSignalMaybeDeliverOnReturn() saved the interrupted process stack
     * pointer before redirecting to the alternate stack. Restore it once, and
     * clear signalActive so later pending signals can be delivered.
     */
    *savedPspPtr = taskPtr->signalSavedPsp;
    RK_CR_ENTER
    taskPtr->signalActive = RK_FALSE;
    taskPtr->signalSavedPsp = NULL;
    RK_CR_EXIT

    return (RK_ERR_SUCCESS);
}

static RK_BOOL kSignalValid_(RK_SIGNAL const signal)
{
    return (((signal >= 1UL) && (signal <= (RK_SIGNAL)RK_CONF_SIGNAL_MAX)) ?
                RK_TRUE :
                RK_FALSE);
}

static RK_SIGNAL kSignalBit_(RK_SIGNAL const signal)
{
    return ((RK_SIGNAL)1UL << (UINT)(signal - 1UL));
}

static RK_BOOL kSignalAnyHandlerRegistered_(RK_TCB const *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return (RK_FALSE);
    }

    for (UINT i = 0U; i < RK_CONF_SIGNAL_MAX; i++)
    {
        if (taskPtr->signalHandler[i] != NULL)
        {
            return (RK_TRUE);
        }
    }

    return (RK_FALSE);
}

static RK_BOOL kSignalUserTask_(RK_TCB const *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return (RK_FALSE);
    }

    return (((taskPtr->savedControl & 0x1UL) != 0UL) ? RK_TRUE : RK_FALSE);
}

static RK_BOOL kSignalRangesOverlap_(UINTPTR const baseA,
                                     ULONG const bytesA,
                                     UINTPTR const baseB,
                                     ULONG const bytesB)
{
    UINTPTR const endA = baseA + (UINTPTR)bytesA;
    UINTPTR const endB = baseB + (UINTPTR)bytesB;

    if ((bytesA == 0UL) || (bytesB == 0UL))
    {
        return (RK_FALSE);
    }

    if ((endA < baseA) || (endB < baseB))
    {
        return (RK_TRUE);
    }

    return (((baseA < endB) && (baseB < endA)) ? RK_TRUE : RK_FALSE);
}

static RK_BOOL kSignalAltStackDisjoint_(RK_TCB const *const taskPtr,
                                        VOID const *const altStackBasePtr,
                                        ULONG const altStackBytes)
{
    UINTPTR const altBase = (UINTPTR)altStackBasePtr;

    if ((taskPtr != NULL) && (taskPtr->stackBufPtr != NULL) &&
        (taskPtr->stackSize <= (RK_ULONG_MAX / (ULONG)sizeof(RK_STACK))))
    {
        ULONG const stackBytes =
            taskPtr->stackSize * (ULONG)sizeof(RK_STACK);

        if (kSignalRangesOverlap_(altBase, altStackBytes,
                                  (UINTPTR)taskPtr->stackBufPtr,
                                  stackBytes) == RK_TRUE)
        {
            return (RK_FALSE);
        }
    }

    for (UINT i = 0U; i < RK_NTHREADS; i++)
    {
        RK_TCB const *const otherPtr = RK_gTaskHandleByPid[i];

        if ((otherPtr == NULL) || (otherPtr->init != RK_TRUE))
        {
            continue;
        }

        if ((otherPtr->stackBufPtr != NULL) &&
            (otherPtr->stackSize <=
             (RK_ULONG_MAX / (ULONG)sizeof(RK_STACK))))
        {
            ULONG const stackBytes =
                otherPtr->stackSize * (ULONG)sizeof(RK_STACK);

            if (kSignalRangesOverlap_(
                    altBase, altStackBytes, (UINTPTR)otherPtr->stackBufPtr,
                    stackBytes) == RK_TRUE)
            {
                return (RK_FALSE);
            }
        }

        if ((otherPtr != taskPtr) &&
            (otherPtr->signalAltStackBasePtr != NULL) &&
            (kSignalRangesOverlap_(
                 altBase, altStackBytes,
                 (UINTPTR)otherPtr->signalAltStackBasePtr,
                 otherPtr->signalAltStackBytes) == RK_TRUE))
        {
            return (RK_FALSE);
        }
    }

    return (RK_TRUE);
}

static RK_SIGNAL kSignalDeliverableMask_(RK_TCB const *const taskPtr)
{
    RK_SIGNAL deliverable = 0UL;

    if ((taskPtr == NULL) || (taskPtr->signalActive == RK_TRUE) ||
        (kSignalUserTask_(taskPtr) == RK_FALSE))
    {
        return (0UL);
    }

    /*
     * Signals are delivered only to unprivileged task context, and only one
     * signal handler may be active per task. Drop bits that no longer have a
     * handler so stale pending state cannot redirect execution.
     */
    deliverable = taskPtr->signalPending & taskPtr->signalEnabledMask;
    for (UINT i = 0U; i < RK_CONF_SIGNAL_MAX; i++)
    {
        RK_SIGNAL const bit = ((RK_SIGNAL)1UL << i);

        if (((deliverable & bit) != 0UL) &&
            (taskPtr->signalHandler[i] == NULL))
        {
            deliverable &= ~bit;
        }
    }

    return (deliverable);
}

static RK_BOOL kSignalAltStackValid_(RK_TCB const *const taskPtr,
                                     VOID *const altStackBasePtr,
                                     ULONG const altStackBytes)
{
    UINTPTR const base = (UINTPTR)altStackBasePtr;
    UINTPTR const top = base + altStackBytes;
    /* altStackBytes is bytes; RK_CONF_MIN_STACKSIZE is words. */
    ULONG const minStackBytes =
        ((ULONG)RK_CONF_MIN_STACKSIZE * (ULONG)sizeof(RK_STACK));

    if ((taskPtr == NULL) || (altStackBasePtr == NULL) ||
        (altStackBytes < minStackBytes) ||
        (top < base) || ((top & 0x7UL) != 0UL))
    {
        return (RK_FALSE);
    }

    if (kSignalAltStackDisjoint_(taskPtr, altStackBasePtr,
                                 altStackBytes) != RK_TRUE)
    {
        return (RK_FALSE);
    }

    return (kMpuUserWriteValid(taskPtr, altStackBasePtr, altStackBytes));
}

static inline RK_ERR kSignalPublicReadyErr_(RK_ERR const err)
{
    if ((err == RK_ERR_RESCHED_PENDING) ||
        (err == RK_ERR_RESCHED_NOT_NEEDED))
    {
        return (RK_ERR_SUCCESS);
    }

    return (err);
}

RK_ERR kSignalHandlerSet(RK_SIGNAL const signal,
                         RK_SIGNAL_HANDLER const handler,
                         VOID *const altStackBasePtr,
                         ULONG const altStackBytes)
{
    RK_TCB *taskPtr;
    RK_SIGNAL bit;
    RK_CR_AREA

    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_SIGNAL_HANDLER_SET, (ULONG)signal,
            (ULONG)(UINTPTR)kSignalHandlerPtr_(handler),
            (ULONG)(UINTPTR)altStackBasePtr,
            altStackBytes));
    }

    taskPtr = RK_gRunPtr;

    if ((taskPtr == NULL) || (kSignalValid_(signal) == RK_FALSE) ||
        (kSignalUserTask_(taskPtr) == RK_FALSE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    bit = kSignalBit_(signal);

    if (handler == NULL)
    {
        /* NULL handler unregisters the signal and clears any stale instance. */
        RK_CR_ENTER
        taskPtr->signalHandler[signal - 1UL] = NULL;
        taskPtr->signalEnabledMask &= ~bit;
        taskPtr->signalPending &= ~bit;
        if (kSignalAnyHandlerRegistered_(taskPtr) == RK_FALSE)
        {
            taskPtr->signalAltStackBasePtr = NULL;
            taskPtr->signalAltStackBytes = 0UL;
        }
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    if ((kSignalExceptionFrameSupported_() != RK_TRUE) ||
        (kMpuUserFunctionValid(kSignalHandlerPtr_(handler)) != RK_TRUE) ||
        (kSignalAltStackValid_(taskPtr, altStackBasePtr,
                               altStackBytes) != RK_TRUE))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    RK_CR_ENTER
    taskPtr->signalAltStackBasePtr = altStackBasePtr;
    taskPtr->signalAltStackBytes = altStackBytes;
    taskPtr->signalHandler[signal - 1UL] = handler;
    taskPtr->signalEnabledMask |= bit;
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSignalSend(RK_TASK_HANDLE const taskHandle, RK_SIGNAL const signal)
{
    RK_TCB *taskPtr = NULL;
    RK_ERR err = RK_ERR_SUCCESS;
    RK_SIGNAL bit;
    RK_CR_AREA

    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SIGNAL_SEND,
                                        (ULONG)(UINTPTR)taskHandle,
                                        (ULONG)signal, 0UL, 0UL));
    }

    if (kSignalValid_(signal) == RK_FALSE)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    err = kTaskHandleResolve(taskHandle, &taskPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    bit = kSignalBit_(signal);

    /*
     * Sending does not run user code immediately. It records pending state and
     * lets the scheduler arrange a safe return boundary for delivery.
     */
    RK_CR_ENTER
    if (taskPtr->signalHandler[signal - 1UL] != NULL)
    {
        taskPtr->signalPending |= bit;
        if ((kSignalDeliverableMask_(taskPtr) & bit) != 0UL)
        {
            err = kTaskSignalReady(taskPtr);
        }
    }
    RK_CR_EXIT
    return (kSignalPublicReadyErr_(err));
}

RK_ERR kSignalMaskSet(RK_SIGNAL const enabledMask)
{
    RK_TCB *taskPtr;
    RK_CR_AREA

    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SIGNAL_MASK_SET,
                                        (ULONG)enabledMask, 0UL, 0UL, 0UL));
    }

    taskPtr = RK_gRunPtr;

    if (taskPtr == NULL)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    RK_CR_ENTER
    /* Ignore bits outside the configured signal set. */
    taskPtr->signalEnabledMask = enabledMask & RK_ALL_SIGNALS;
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSignalReturn(VOID)
{
    VOID *savedPsp = NULL;
    VOID *returnPsp = NULL;
    RK_ERR err;

    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(RK_SYSCALL_SIGNAL_RETURN,
                                        0UL, 0UL, 0UL, 0UL));
    }

    err = kSignalReturnKernel_(&savedPsp);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    /*
     * Continue from the interrupted frame. If another enabled signal arrived
     * while the handler was active, build the next alternate-stack frame from
     * that restored context now: sequential delivery, not nesting.
     */
    returnPsp = kSignalMaybeDeliverOnReturn((RK_EXCEPTION_FRAME *)savedPsp);
    RK_ASM volatile("MSR PSP, %0" :: "r"(returnPsp) : "memory");
    return (RK_ERR_SUCCESS);
}

VOID kSignalTaskCleanup(RK_TCB *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return;
    }

    taskPtr->signalPending = 0UL;
    taskPtr->signalEnabledMask = 0UL;
    taskPtr->signalAltStackBasePtr = NULL;
    taskPtr->signalAltStackBytes = 0UL;
    taskPtr->signalActive = RK_FALSE;
    taskPtr->signalSavedPsp = NULL;
    RK_MEMSET(taskPtr->signalHandler, 0, sizeof(taskPtr->signalHandler));
}

VOID *kSignalMaybeDeliverOnReturn(RK_EXCEPTION_FRAME *const framePtr)
{
    RK_TCB *const taskPtr = RK_gRunPtr;
    RK_SIGNAL deliverable;
    RK_SIGNAL signal = RK_SIGNAL_NONE;
    RK_SIGNAL bit = 0UL;
    RK_EXCEPTION_FRAME *signalFramePtr;
    UINTPTR stackTop;
    RK_SIGNAL_HANDLER handler;
    RK_CR_AREA

    if ((taskPtr == NULL) || (framePtr == NULL) ||
        (kSignalExceptionFrameSupported_() != RK_TRUE))
    {
        return (framePtr);
    }

    RK_CR_ENTER
    deliverable = kSignalDeliverableMask_(taskPtr);
    if (deliverable == 0UL)
    {
        RK_CR_EXIT
        return (framePtr);
    }

    for (UINT i = 0U; i < RK_CONF_SIGNAL_MAX; i++)
    {
        bit = ((RK_SIGNAL)1UL << i);
        if ((deliverable & bit) != 0UL)
        {
            signal = (RK_SIGNAL)(i + 1U);
            break;
        }
    }

    handler = taskPtr->signalHandler[signal - 1UL];
    taskPtr->signalPending &= ~bit;
    taskPtr->signalActive = RK_TRUE;
    taskPtr->signalSavedPsp = framePtr;

    /*
     * Build the frame that hardware exception return expects to pop. The PSP is
     * switched to this frame by the caller, so returning from SVC enters the
     * trampoline in unprivileged thread mode on the alternate signal stack.
     */
    stackTop = ((UINTPTR)taskPtr->signalAltStackBasePtr) +
               taskPtr->signalAltStackBytes;
    stackTop &= ~((UINTPTR)0x7UL);
    signalFramePtr =
        (RK_EXCEPTION_FRAME *)(stackTop - sizeof(RK_EXCEPTION_FRAME));

    RK_MEMSET(signalFramePtr, 0, sizeof(RK_EXCEPTION_FRAME));
    signalFramePtr->r0 = (ULONG)signal;
    signalFramePtr->r1 = kSignalHandlerArg_(handler);
    signalFramePtr->lr = kSignalReturnThumbAddr_();
    signalFramePtr->pc = kSignalTrampolineThumbAddr_();
    signalFramePtr->xpsr = RK_SIGNAL_XPSR_THUMB;
    RK_CR_EXIT

    return (signalFramePtr);
}

VOID kSignalMaybeDeliverOnSvcExit(RK_EXCEPTION_FRAME *const framePtr)
{
    VOID *const returnFramePtr = kSignalMaybeDeliverOnReturn(framePtr);

    RK_ASM volatile("MSR PSP, %0" :: "r"(returnFramePtr) : "memory");
}
