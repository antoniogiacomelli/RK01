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
 *   Core logging service. Unprivileged callers format bounded text locally,
 *   publish records through kernel-managed storage, and the privileged logger
 *   task drains those records to the board console.
 */

#include <klogger.h>

#if (CONF_LOGGER == 1)
#include <kapi.h>
#include <kconsole.h>
#include <stdio.h>
#include <kstring.h>
#include <kmem.h>
#include <kmesgq.h>
#include <ksch.h>
#include <ksyscall.h>
#include <ktrace.h>
#endif
#include <stdarg.h>

#if (CONF_LOGGER == 0)
VOID kLogInit(RK_PRIO priority)
{
    K_UNUSE(priority);
}
VOID kLogWrite(UINT level, const char *fmt, ...)
{
    (void)level;
    va_list args;
    va_start(args, fmt);
    (void)(args);
    (void)(fmt);
    va_end(args);
}

VOID kLogNormalOutputSet(RK_BOOL enabled)
{
    K_UNUSE(enabled);
}

RK_BOOL kLogNormalOutputGet(VOID)
{
    return (RK_FALSE);
}
#else


/* Standard log structure. */
struct log
{
    RK_TICK t;      /* timestamp */
    CHAR s[LOGLEN]; /* formatted string */
    UINT level;     /* level 0=message, 1=fault */
} K_ALIGN(4);

typedef struct log Log_t;

static Log_t logBufPool[LOGPOOLSIZ] K_ALIGN(4);

/* Backing buffer for the logger queue; messages are one-word pointers. */
RK_DECLARE_MESG_QUEUE_BUF(logQBuf, VOID *, LOGPOOLSIZ)

/* Logger mail queue. */
static RK_MESG_QUEUE logQ;
static RK_MESG_QUEUE_HANDLE logQHandle =
    (RK_MESG_QUEUE_HANDLE)(UINTPTR)&logQ;

/* SVC log submission cannot pass an MSP local variable through the public
 * queue API, because the queue syscall validator expects user-readable memory.
 */
static VOID *logSendSlot RK_SHARED_RAM_ATTR;

/* Logger memory allocator. */
static RK_MEM_PARTITION qMem;

/* Logger task handle and stack. */
static RK_TASK_HANDLE logTaskHandle;
static RK_STACK logstack[LOG_STACKSIZE] RK_PRIVILEGED_TASK_STACK_ATTR(LOG_STACKSIZE);
static volatile RK_BOOL logNormalOutputEnabled = RK_TRUE;

static inline VOID logPrintf_(const char *fmt, ...)
{

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}
static ULONG errLogSend = 0UL; /* Increases when a log send fails. */
static ULONG errLogPool = 0UL; /* increases when alloc returns null */

VOID kLogNormalOutputSet(RK_BOOL const enabled)
{
    logNormalOutputEnabled = (enabled == RK_FALSE) ? RK_FALSE : RK_TRUE;
}

RK_BOOL kLogNormalOutputGet(VOID)
{
    return (logNormalOutputEnabled);
}

static VOID logCopyText_(CHAR *const dstPtr,
                         ULONG const dstBytes,
                         CHAR const *srcPtr)
{
    ULONG i = 0UL;

    if ((dstPtr == NULL) || (dstBytes == 0UL))
    {
        return;
    }

    if (srcPtr == NULL)
    {
        dstPtr[0] = '\0';
        return;
    }

    while (((i + 1UL) < dstBytes) && (srcPtr[i] != '\0'))
    {
        dstPtr[i] = srcPtr[i];
        i++;
    }

    dstPtr[i] = '\0';
}

static VOID logAppendChar_(CHAR *const textPtr,
                           ULONG const textBytes,
                           ULONG *const posPtr,
                           CHAR const ch)
{
    if ((textPtr == NULL) || (posPtr == NULL) || (textBytes == 0UL))
    {
        return;
    }

    if ((*posPtr + 1UL) < textBytes)
    {
        textPtr[*posPtr] = ch;
    }

    if (*posPtr < RK_ULONG_MAX)
    {
        (*posPtr)++;
    }
}

static VOID logAppendText_(CHAR *const textPtr,
                           ULONG const textBytes,
                           ULONG *const posPtr,
                           CHAR const *srcPtr)
{
    if (srcPtr == NULL)
    {
        srcPtr = "(null)";
    }

    while (*srcPtr != '\0')
    {
        logAppendChar_(textPtr, textBytes, posPtr, *srcPtr);
        srcPtr++;
    }
}

static VOID logAppendUnsigned_(CHAR *const textPtr,
                               ULONG const textBytes,
                               ULONG *const posPtr,
                               ULONG value,
                               UINT const base,
                               RK_BOOL const upper)
{
    CHAR tmp[sizeof(ULONG) * 8U];
    ULONG n = 0UL;

    if ((base != 10U) && (base != 16U))
    {
        return;
    }

    do
    {
        ULONG const digit = value % (ULONG)base;
        tmp[n] = (CHAR)((digit < 10UL)
                            ? ('0' + digit)
                            : ((upper == RK_TRUE) ? ('A' + (digit - 10UL))
                                                  : ('a' + (digit - 10UL))));
        value /= (ULONG)base;
        n++;
    } while ((value != 0UL) && (n < (ULONG)sizeof(tmp)));

    while (n != 0UL)
    {
        n--;
        logAppendChar_(textPtr, textBytes, posPtr, tmp[n]);
    }
}

static VOID logAppendSigned_(CHAR *const textPtr,
                             ULONG const textBytes,
                             ULONG *const posPtr,
                             LONG const value)
{
    ULONG magnitude;

    if (value < 0L)
    {
        logAppendChar_(textPtr, textBytes, posPtr, '-');
        magnitude = ((ULONG)(-(value + 1L))) + 1UL;
    }
    else
    {
        magnitude = (ULONG)value;
    }

    logAppendUnsigned_(textPtr, textBytes, posPtr, magnitude, 10U, RK_FALSE);
}

static RK_BOOL logIsDigit_(CHAR const ch)
{
    return (((ch >= '0') && (ch <= '9')) ? RK_TRUE : RK_FALSE);
}

static VOID logSkipPrintfDecoration_(CHAR const *const fmt,
                                     ULONG *const idxPtr)
{
    if ((fmt == NULL) || (idxPtr == NULL))
    {
        return;
    }

    while ((fmt[*idxPtr] == '-') || (fmt[*idxPtr] == '+') ||
           (fmt[*idxPtr] == ' ') || (fmt[*idxPtr] == '#') ||
           (fmt[*idxPtr] == '0'))
    {
        (*idxPtr)++;
    }

    while (logIsDigit_(fmt[*idxPtr]) == RK_TRUE)
    {
        (*idxPtr)++;
    }

    if (fmt[*idxPtr] == '.')
    {
        (*idxPtr)++;
        while (logIsDigit_(fmt[*idxPtr]) == RK_TRUE)
        {
            (*idxPtr)++;
        }
    }
}

static VOID logFormatText_(CHAR *const textPtr,
                           ULONG const textBytes,
                           CHAR const *const fmt,
                           va_list args)
{
    ULONG pos = 0UL;
    ULONG i = 0UL;

    if ((textPtr == NULL) || (textBytes == 0UL))
    {
        return;
    }

    if (fmt == NULL)
    {
        logCopyText_(textPtr, textBytes, "[LOG FORMAT NULL]");
        return;
    }

    while (fmt[i] != '\0')
    {
        if (fmt[i] != '%')
        {
            logAppendChar_(textPtr, textBytes, &pos, fmt[i]);
            i++;
            continue;
        }

        i++;
        if (fmt[i] == '%')
        {
            logAppendChar_(textPtr, textBytes, &pos, '%');
            i++;
            continue;
        }

        logSkipPrintfDecoration_(fmt, &i);

        RK_BOOL longArg = RK_FALSE;
        if (fmt[i] == 'l')
        {
            longArg = RK_TRUE;
            i++;
            if (fmt[i] == 'l')
            {
                i++;
            }
        }

        switch (fmt[i])
        {
            case 's':
                logAppendText_(textPtr, textBytes, &pos,
                               va_arg(args, CHAR const *));
                break;

            case 'c':
                logAppendChar_(textPtr, textBytes, &pos,
                               (CHAR)va_arg(args, INT));
                break;

            case 'd':
            case 'i':
                logAppendSigned_(
                    textPtr, textBytes, &pos,
                    longArg == RK_TRUE ? va_arg(args, LONG)
                                       : (LONG)va_arg(args, INT));
                break;

            case 'u':
                logAppendUnsigned_(
                    textPtr, textBytes, &pos,
                    longArg == RK_TRUE ? va_arg(args, ULONG)
                                       : (ULONG)va_arg(args, UINT),
                    10U, RK_FALSE);
                break;

            case 'x':
            case 'X':
                logAppendUnsigned_(
                    textPtr, textBytes, &pos,
                    longArg == RK_TRUE ? va_arg(args, ULONG)
                                       : (ULONG)va_arg(args, UINT),
                    16U, fmt[i] == 'X' ? RK_TRUE : RK_FALSE);
                break;

            case 'p':
                logAppendText_(textPtr, textBytes, &pos, "0x");
                logAppendUnsigned_(textPtr, textBytes, &pos,
                                   (ULONG)(UINTPTR)va_arg(args, VOID *),
                                   16U, RK_FALSE);
                break;

            case '\0':
                logAppendChar_(textPtr, textBytes, &pos, '%');
                continue;

            default:
                logAppendChar_(textPtr, textBytes, &pos, '%');
                logAppendChar_(textPtr, textBytes, &pos, fmt[i]);
                break;
        }

        i++;
    }

    if (pos >= textBytes)
    {
        textPtr[textBytes - 1UL] = '\0';
    }
    else
    {
        textPtr[pos] = '\0';
    }
}

static RK_BOOL logRecordValid_(VOID const *const recordPtr)
{
    BYTE const *const poolBeginPtr = (BYTE const *)&logBufPool[0];
    BYTE const *const poolEndPtr =
        poolBeginPtr + (sizeof(logBufPool[0]) * LOGPOOLSIZ);
    BYTE const *const bytePtr = (BYTE const *)recordPtr;
    BYTE const *freePtr;

    if ((recordPtr == NULL) || (qMem.init != RK_TRUE))
    {
        return (RK_FALSE);
    }

    if ((bytePtr < poolBeginPtr) || (bytePtr >= poolEndPtr))
    {
        return (RK_FALSE);
    }

    if (((ULONG)(bytePtr - poolBeginPtr) % sizeof(logBufPool[0])) != 0UL)
    {
        return (RK_FALSE);
    }

    freePtr = qMem.freeListPtr;
    for (ULONG i = 0UL; (i < qMem.nFreeBlocks) && (freePtr != NULL); i++)
    {
        if (freePtr == bytePtr)
        {
            return (RK_FALSE);
        }

        freePtr = *(BYTE **)freePtr;
    }

    return (RK_TRUE);
}

static RK_ERR logSubmit_(UINT const level, CHAR const *const textPtr)
{
    Log_t *const logPtr = (Log_t *)kMemPartitionAlloc(&qMem);
    VOID *sendPtr;
    VOID **sendPPtr = &sendPtr;
    RK_BARRIER

    if (logPtr == NULL)
    {
        ++errLogPool;
#if (CONF_LOG_ERROR == ON)
        if (logNormalOutputEnabled == RK_TRUE)
        {
            kPuts("E\r\n");
        }
#endif
        return (RK_ERR_BUFFER_EMPTY);
    }

    logPtr->level = level;
    logPtr->t = kTickGetMs();
    logCopyText_(logPtr->s, sizeof(logPtr->s), textPtr);
    sendPtr = logPtr;

    /* Do not block while handling a syscall-origin logger write. */
    if (RK_gSyscallThreadModeActive != 0U)
    {
        logSendSlot = sendPtr;
        sendPPtr = &logSendSlot;
    }

    if (kMesgQueueSend(logQHandle, sendPPtr, RK_NO_WAIT) != RK_ERR_SUCCESS)
    {
        ++errLogSend;

        RK_ERR const err = kMemPartitionFree(&qMem, logPtr);
        K_ASSERT(err == RK_ERR_SUCCESS);
        return (RK_ERR_BUFFER_FULL);
    }

    return (RK_ERR_SUCCESS);
}

static RK_ERR logWrite_(UINT const level, CHAR const *const textPtr)
{
    if (level == LOG_LEVEL_FAULT)
    {
        logPrintf_("@%lu ms %s", kTickGetMs(),
                   (textPtr != NULL) ? textPtr : "");
        RK_ABORT
    }

    return (logSubmit_(level, textPtr));
}

static RK_ERR logVwrite_(UINT const level,
                         CHAR const *const fmt,
                         va_list args)
{
    CHAR text[LOGLEN];

    logFormatText_(text, sizeof(text), fmt, args);
    return (logWrite_(level, text));
}

static RK_ERR logConsume_(VOID *const recordPtr)
{
    if (logRecordValid_(recordPtr) == RK_FALSE)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    Log_t *const logPtr = (Log_t *)recordPtr;
    logPrintf_("%8lu ms :: %s \r\n", logPtr->t, logPtr->s);

    return (kMemPartitionFree(&qMem, recordPtr));
}

/* Formatted string input. */
VOID kLogWrite(UINT level, const char *fmt, ...)
{
    RK_ERR err;
    va_list args;

    va_start(args, fmt);

    if (kSyscallRequired() == RK_TRUE)
    {
        CHAR text[LOGLEN];

        logFormatText_(text, sizeof(text), fmt, args);
        err = (RK_ERR)kSyscallInvoke4(RK_SYSCALL_LOG_WRITE,
                                      (ULONG)level,
                                      (ULONG)(UINTPTR)text,
                                      0UL,
                                      0UL);
    }
    else
    {
        err = logVwrite_(level, fmt, args);
    }

    va_end(args);

    K_UNUSE(err);
    if (level == LOG_LEVEL_FAULT)
    {
        RK_ABORT
    }
}

static VOID LoggerTask(VOID *args)
{
    RK_UNUSEARGS
    while (1)
    {

        VOID *recvPtr = NULL;
        while (kMesgQueueRecv(logQHandle, &recvPtr, RK_WAIT_FOREVER) ==
               RK_ERR_SUCCESS)
        {
            K_ASSERT(recvPtr != NULL);
            while ((logRecordValid_(recvPtr) == RK_TRUE) &&
                   (((Log_t *)recvPtr)->level != LOG_LEVEL_FAULT) &&
                   (logNormalOutputEnabled == RK_FALSE))
            {
                kSleepDelay(1U);
            }

            RK_ERR err;
            if (kSyscallRequired() == RK_TRUE)
            {
                err = (RK_ERR)kSyscallInvoke4(
                    RK_SYSCALL_LOG_CONSUME, (ULONG)(UINTPTR)recvPtr,
                    0UL, 0UL, 0UL);
            }
            else
            {
                err = logConsume_(recvPtr);
            }

            K_ASSERT(err == RK_ERR_SUCCESS);
        }
    }
}

VOID kLogInit(RK_PRIO priority)
{
    logQHandle = (RK_MESG_QUEUE_HANDLE)(UINTPTR)&logQ;

    RK_ERR err = kMemPartitionInitGlobalScope(&qMem, logBufPool,
                                              sizeof(Log_t), LOGPOOLSIZ);
    K_ASSERT(err == RK_ERR_SUCCESS);
    err = kTraceNameObject(&qMem, "LogMem");
    K_ASSERT(err == RK_ERR_SUCCESS);

    err = kMesgQueueInit(&logQ, logQBuf, RK_MESGQ_MESG_SIZE(VOID *),
                         LOGPOOLSIZ);
    K_ASSERT(err == RK_ERR_SUCCESS);
    err = kObjHeaderScopeSet(&logQ.header, RK_SCOPE_KERNEL_GLOBAL, NULL);
    K_ASSERT(err == RK_ERR_SUCCESS);
    err = kTraceNameObject(logQHandle, "LogQ");
    K_ASSERT(err == RK_ERR_SUCCESS);

    err = kTaskInitPrivileged(&logTaskHandle, LoggerTask, RK_NO_ARGS,
                              "LogTsk", logstack, LOG_STACKSIZE, priority,
                              RK_PREEMPT);
    K_ASSERT(err == RK_ERR_SUCCESS);
}

static RK_ERR logUserTextReadValid_(CHAR const *const textPtr)
{
    if (textPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    return ((kMpuUserReadValid(RK_gRunPtr, textPtr, LOGLEN) == RK_TRUE)
                ? RK_ERR_SUCCESS
                : RK_ERR_INVALID_PARAM);
}

RK_BOOL kLoggerSyscallDispatch(RK_EXCEPTION_FRAME *const framePtr,
                               ULONG const callNumber,
                               ULONG const arg0,
                               ULONG const arg1,
                               ULONG const arg2,
                               ULONG const arg3)
{
    RK_ERR ret;

    K_UNUSE(arg2);
    K_UNUSE(arg3);

    if (framePtr == NULL)
    {
        return (RK_FALSE);
    }

    switch (callNumber)
    {
        case RK_SYSCALL_LOG_WRITE:
            ret = logUserTextReadValid_((CHAR const *)(UINTPTR)arg1);
            if (ret == RK_ERR_SUCCESS)
            {
                ret = logWrite_((UINT)arg0, (CHAR const *)(UINTPTR)arg1);
            }
            framePtr->r0 = (ULONG)ret;
            return (RK_TRUE);

        case RK_SYSCALL_LOG_CONSUME:
            ret = logConsume_((VOID *)(UINTPTR)arg0);
            framePtr->r0 = (ULONG)ret;
            return (RK_TRUE);

        case RK_SYSCALL_LOG_FORMAT:
            ret = RK_ERR_INVALID_PARAM;
            framePtr->r0 = (ULONG)ret;
            return (RK_TRUE);

        default:
            break;
    }

    return (RK_FALSE);
}

#endif
