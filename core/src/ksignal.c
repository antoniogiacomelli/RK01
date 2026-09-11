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

    *savedPspPtr = taskPtr->signalSavedPsp;
    RK_CR_ENTER
    taskPtr->signalActive = RK_FALSE;
    taskPtr->signalSavedPsp = NULL;
    RK_CR_EXIT

    return (RK_ERR_SUCCESS);
}

static RK_BOOL kSignalValid_(RK_SIGNAL const signal)
{
    return (((signal >= 1UL) && (signal <= (RK_SIGNAL)RK_SIGNAL_MAX)) ?
                RK_TRUE :
                RK_FALSE);
}

static RK_SIGNAL kSignalBit_(RK_SIGNAL const signal)
{
    return ((RK_SIGNAL)1UL << (UINT)(signal - 1UL));
}

static RK_BOOL kSignalUserTask_(RK_TCB const *const taskPtr)
{
    if (taskPtr == NULL)
    {
        return (RK_FALSE);
    }

    return (((taskPtr->savedControl & 0x1UL) != 0UL) ? RK_TRUE : RK_FALSE);
}

static RK_SIGNAL kSignalDeliverableMask_(RK_TCB const *const taskPtr)
{
    RK_SIGNAL deliverable = 0UL;

    if ((taskPtr == NULL) || (taskPtr->signalActive == RK_TRUE) ||
        (kSignalUserTask_(taskPtr) == RK_FALSE))
    {
        return (0UL);
    }

    deliverable = taskPtr->signalPending & taskPtr->signalEnabledMask;
    for (UINT i = 0U; i < RK_SIGNAL_MAX; i++)
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

    if ((taskPtr == NULL) || (altStackBasePtr == NULL) ||
        (altStackBytes < RK_SIGNAL_ALT_STACK_MIN_BYTES) ||
        (top < base) || ((top & 0x7UL) != 0UL))
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
        RK_CR_ENTER
        taskPtr->signalHandler[signal - 1UL] = NULL;
        taskPtr->signalEnabledMask &= ~bit;
        taskPtr->signalPending &= ~bit;
        RK_CR_EXIT
        return (RK_ERR_SUCCESS);
    }

    if ((kMpuUserFunctionValid(kSignalHandlerPtr_(handler)) != RK_TRUE) ||
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

    RK_CR_ENTER
    if (taskPtr->signalHandler[signal - 1UL] != NULL)
    {
        taskPtr->signalPending |= bit;
        err = kTaskSignalReady(taskPtr);
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
    taskPtr->signalEnabledMask = enabledMask & RK_ALL_SIGNALS;
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

RK_ERR kSignalReturn(VOID)
{
    VOID *savedPsp = NULL;
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

    RK_ASM volatile("MSR PSP, %0" :: "r"(savedPsp) : "memory");
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

    if ((taskPtr == NULL) || (framePtr == NULL))
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

    for (UINT i = 0U; i < RK_SIGNAL_MAX; i++)
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
