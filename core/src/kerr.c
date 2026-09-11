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
 *   Central error and fatal-fault reporting. It also hosts compile-time checks
 *   that catch unsupported architecture and configuration combinations early.
 */

#define RK_SOURCE_CODE

#include <kerr.h>
#include <ksyscall.h>

#define RK_FAULT_TRACE_MAGIC (0x524B4654UL)

#ifndef RK_BUILD_COOKIE
#define RK_BUILD_COOKIE (0UL)
#endif

/*** Compile time errors ****/
#if defined(__ARM_ARCH_7EM__) /* Cortex-M4 / M7 */
#define ARCH_CM_7EM 1
#elif defined(__ARM_ARCH_7M__) /* Cortex-M3       */
#define ARCH_CM_7M 1
#elif defined(__ARM_ARCH_8M_MAIN__) /* Cortex-M33 / M55 */
#define ARCH_CM_8M_MAIN 1
#elif defined(__ARM_ARCH_6M__) /* Cortex-M0/M0+/ */
#define ARCH_CM_6M 1
#else
#error "Unsupported Cortex-M architecture—check your -mcpu/-march"
#endif

#ifndef __GNUC__
#error "You need GCC as your compiler!"
#endif

#ifndef RK_VALID_VERSION
#error "Missing RK01 version"
#endif

#if (RK_CONF_MIN_PRIO > 31)
#error "Invalid minimal effective priority. (Max numerical value: 31)"
#endif

/******************************************************************************
 * ERROR HANDLING
 ******************************************************************************/
#if (RK_CONF_FAULT == ON)

#if (RK_CONF_FAULT_PRINT_STDERR == ON)
#define RK_FAULT_LIST(F)                                                       \
    F(RK_GENERIC_FAULT)                                                        \
    F(RK_FAULT_READY_QUEUE)                                                    \
    F(RK_FAULT_OBJ_NULL)                                                       \
    F(RK_FAULT_OBJ_NOT_INIT)                                                   \
    F(RK_FAULT_OBJ_DOUBLE_INIT)                                                \
    F(RK_FAULT_HAS_OWNER)                                                      \
    F(RK_FAULT_TASK_INVALID_PRIO)                                              \
    F(RK_FAULT_UNLOCK_OWNED_MUTEX)                                             \
    F(RK_FAULT_MUTEX_REC_LOCK)                                                 \
    F(RK_FAULT_MUTEX_NOT_LOCKED)                                               \
    F(RK_FAULT_INVALID_ISR_PRIMITIVE)                                          \
    F(RK_FAULT_TASK_INVALID_STATE)                                             \
    F(RK_FAULT_INVALID_OBJ)                                                    \
    F(RK_FAULT_INVALID_PARAM)                                                  \
    F(RK_FAULT_INVALID_PHASE)                                                  \
    F(RK_FAULT_INVALID_TIMEOUT)                                                \
    F(RK_FAULT_CHANNEL_NOT_ACTIVE)                                             \
    F(RK_FAULT_SYNCH_CALL_NOT_ACTIVE)                                          \
    F(RK_FAULT_MESG_INVALID_STATE)                                             \
    F(RK_FAULT_MEM_ACCESS)                                                     \
    F(RK_FAULT_STACK_OVERFLOW)                                                 \
    F(RK_FAULT_TASK_COUNT_MISMATCH)                                            \
    F(RK_FAULT_KERNEL_VERSION)                                                 \
    F(RK_FAULT_APP_CRASH)                                                      \
    F(RK_FAULT_INIT_KERNEL)                                                    \
    F(RK_FAULT_TASK_COUNT_MISMATCH)                                            \
    F(RK_FAULT_MISSING_APPLICATION_INIT)

typedef struct
{
    RK_FAULT faultCode;
    const char *faultStr;
} RK_FAULT_NAME;

#define RK_FAULT_ENTRY(F) {F, #F},
static const RK_FAULT_NAME kFaultNames[] = {RK_FAULT_LIST(RK_FAULT_ENTRY)
#undef RK_FAULT_ENTRY
};

static inline const char *kStringfyFault_(RK_FAULT code)
{
    for (ULONG idx = 0; idx < sizeof(kFaultNames) / sizeof(kFaultNames[0]);
         ++idx)
    {
        if (kFaultNames[idx].faultCode == code)
        {
            return (kFaultNames[idx].faultStr);
        }
    }
    return ("RK_FAULT_UNKNOWN");
}

#endif

volatile RK_FAULT RK_gFaultID = 0;
volatile struct traceItem RK_gTraceInfo;

VOID kFaultTraceClear(VOID)
{
    RK_gTraceInfo.magic = RK_FAULT_TRACE_MAGIC;
    RK_gTraceInfo.buildCookie = (ULONG)RK_BUILD_COOKIE;
    RK_gTraceInfo.code = 0;
    RK_gTraceInfo.tick = 0UL;
    RK_gTraceInfo.sp = 0U;
    RK_gTraceInfo.task = NULL;
    RK_gTraceInfo.taskID = (BYTE)0xFFU;
    RK_gTraceInfo.lr = 0U;
}

VOID kFaultTraceInit(VOID)
{
    if ((RK_gTraceInfo.magic != RK_FAULT_TRACE_MAGIC) ||
        (RK_gTraceInfo.buildCookie != (ULONG)RK_BUILD_COOKIE))
    {
        kFaultTraceClear();
    }
}

void kErrHandler(RK_FAULT fault) /* generic error handler */
{
    if (kSyscallRequired() == RK_TRUE)
    {
        kSyscallInvoke4(RK_SYSCALL_ERR_HANDLER, (ULONG)fault,
                              0UL, 0UL, 0UL);
        return;
    }

    RK_CR_AREA
    RK_CR_ENTER

    RK_gTraceInfo.magic = RK_FAULT_TRACE_MAGIC;
    RK_gTraceInfo.buildCookie = (ULONG)RK_BUILD_COOKIE;
    RK_gTraceInfo.code = fault;
    RK_gFaultID = fault;
    if (RK_gRunPtr)
    {
        RK_gTraceInfo.task = RK_gRunPtr->taskName;
        RK_gTraceInfo.sp = (UINT)RK_gRunPtr->sp;
        RK_gTraceInfo.taskID = (BYTE)RK_gRunPtr->tid;
    }
    else
    {
        RK_gTraceInfo.task = 0;
        RK_gTraceInfo.sp = 0;
        RK_gTraceInfo.taskID = (BYTE)0xFFU;
    }

    register unsigned lr_value;
    __asm volatile("mov %0, lr" : "=r"(lr_value));
    RK_gTraceInfo.lr = lr_value;
    RK_gTraceInfo.tick = kTickGet();
#if (RK_CONF_FAULT_PRINT_STDERR == ON)
    printf("FATAL: %04x : %s \n\r", RK_gFaultID, kStringfyFault_(RK_gFaultID));
    printf("AT TASK ID: %u PTR: 0x%08lx,\n\r", (UINT)RK_gTraceInfo.taskID,
           (ULONG)RK_gRunPtr);
#endif
    RK_CR_EXIT
    RK_ABORT
}
#else
volatile RK_FAULT RK_gFaultID = 0;
volatile struct traceItem RK_gTraceInfo;

VOID kFaultTraceClear(VOID)
{
    RK_gTraceInfo.magic = RK_FAULT_TRACE_MAGIC;
    RK_gTraceInfo.buildCookie = (ULONG)RK_BUILD_COOKIE;
    RK_gTraceInfo.code = 0;
    RK_gTraceInfo.tick = 0UL;
    RK_gTraceInfo.sp = 0U;
    RK_gTraceInfo.task = NULL;
    RK_gTraceInfo.taskID = (BYTE)0xFFU;
    RK_gTraceInfo.lr = 0U;
}

VOID kFaultTraceInit(VOID)
{
    if ((RK_gTraceInfo.magic != RK_FAULT_TRACE_MAGIC) ||
        (RK_gTraceInfo.buildCookie != (ULONG)RK_BUILD_COOKIE))
    {
        kFaultTraceClear();
    }
}

void kErrHandler(RK_FAULT fault)
{
    (void)fault;
    return;
}
#endif

#ifndef NDEBUG

VOID kPanic(const char *fileName, const int line, const char *fmt, ...)
{
    RK_ASM volatile("CPSID I" : : : "memory");
    printf("@%lums PANIC ! FILE:%s LINE:%d\r\n", kTickGetMs(), fileName, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    RK_ASM volatile("BKPT #0");
    while (1)
    {
        RK_ASM volatile("NOP");
    }
}
#else
VOID kPanic(const char *fileName, const int line, const char *fmt, ...)
{
    (void)fileName;
    (void)line;
    va_list args;
    va_start(args, fmt);
    (void)(args);
    (void)(fmt);
    va_end(args);
}
#endif
