/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/** ARMv7-M core definitions and minimal core-HAL. */
#ifndef RK_COREDEFS_H
#define RK_COREDEFS_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <kexecutive.h>
#include <khal.h>




extern unsigned long RK_gSyTickDiv;
extern unsigned long RK_gSysCoreClock;
extern unsigned long RK_gSysTickInterval;
extern volatile unsigned RK_gSyscallThreadModeActive;

#define RK_CORE_SVC_IRQN ((int)-5)
#define RK_CORE_DEBUGMON_IRQN ((int)-4)
#define RK_CORE_PENDSV_IRQN ((int)-2)
#define RK_CORE_SYSTICK_IRQN ((int)-1)
#define RK_CORE_SVC_EXCEPTION (11U)



void kCoreInit(void);

/* Assembly Helpers */
#define RK_DMB RK_ASM volatile("DMB" :: : "memory");
#define RK_DSB RK_ASM volatile("DSB" :: : "memory");
#define RK_ISB RK_ASM volatile("ISB" :: : "memory");
#define RK_NOP RK_ASM volatile("NOP");
#define RK_WFI RK_ASM volatile("WFI" :: : "memory");
#define RK_DIS_IRQ RK_ASM volatile("CPSID I" :: : "memory");
#define RK_EN_IRQ RK_ASM volatile("CPSIE I" :: : "memory");

#define RK_CR_BASEPRI_VALUE \
    ((unsigned)((RK_CONF_CR_BASEPRI_PRIO << (8U - RK_CONF_NPRIO_BITS)) & 0xFFU))

#if ((RK_CONF_CR_MASK == RK_CONF_CR_BASEPRI) && \
     (RK_CONF_CR_BASEPRI_PRIO == 0U))
#error "RK_CONF_CR_BASEPRI_PRIO must be non-zero when BASEPRI critical sections are selected"
#endif

RK_FORCE_INLINE
static inline unsigned kAtomicLoadU32(volatile unsigned const *const ptr)
{
    unsigned value = *ptr;
    RK_DMB
    return (value);
}

RK_FORCE_INLINE
static inline void kAtomicStoreU32(volatile unsigned *const ptr,
                                   unsigned const value)
{
    RK_DMB
    *ptr = value;
}

RK_FORCE_INLINE
static inline RK_BOOL kAtomicCompareExchangeU32(
    volatile unsigned *const ptr,
    unsigned *const expectedPtr,
    unsigned const desired)
{
    unsigned loaded;
    unsigned status = 1U;
    unsigned const expected = *expectedPtr;

    RK_DMB
    RK_ASM volatile(
        "ldrex %[loaded], [%[addr]]       \n"
        "cmp   %[loaded], %[expected]     \n"
        "bne   1f                         \n"
        "strex %[status], %[desired], [%[addr]]\n"
        "1:                               \n"
        : [loaded] "=&r"(loaded), [status] "+r"(status)
        : [addr] "r"(ptr), [expected] "r"(expected),
          [desired] "r"(desired)
        : "cc", "memory");

    if (loaded != expected)
    {
        RK_ASM volatile("CLREX" ::: "memory");
        *expectedPtr = loaded;
        return (RK_FALSE);
    }

    if (status != 0U)
    {
        return (RK_FALSE);
    }

    RK_DMB
    return (RK_TRUE);
}

RK_FORCE_INLINE
static inline unsigned kEnterCR(void)
{
    unsigned state;
#if (RK_CONF_CR_MASK == RK_CONF_CR_BASEPRI)
    RK_ASM volatile("MRS %0, BASEPRI " : "=r"(state));
    RK_ASM volatile("MSR BASEPRI_MAX, %0" : : "r"(RK_CR_BASEPRI_VALUE) :
                    "memory");
    RK_DSB
    RK_ISB
#else
    RK_ASM volatile("MRS %0, PRIMASK " : "=r"(state));
    RK_DIS_IRQ
#endif
    return (state);
}

RK_FORCE_INLINE
static inline void kExitCR(unsigned state)
{
#if (RK_CONF_CR_MASK == RK_CONF_CR_BASEPRI)
    RK_ASM volatile("MSR BASEPRI, %0" : : "r"(state) : "memory");
    RK_DSB
    RK_ISB
#else
    RK_ASM volatile("MSR PRIMASK, %0" : : "r"(state) : "memory");
#endif
}

#define RK_CR_AREA unsigned RK_crState;
#define RK_CR_ENTER RK_crState = kEnterCR();
#define RK_CR_EXIT kExitCR(RK_crState);

#define RK_PEND_CTXTSWTCH\
    do\
    {\
        RK_REG_SCB_ICSR |= (1U << 28);\
    } while (0);

#define RK_SVC_SYSCALL_IMM (0x00U)
#define RK_SVC_STARTUP_IMM (0xAAU)

#define RK_STUP RK_ASM volatile("SVC #0xAA" ::: "memory");

RK_FORCE_INLINE
static inline ULONG kSyscall4_(ULONG const callNumber,
                               ULONG const arg0,
                               ULONG const arg1,
                               ULONG const arg2,
                               ULONG const arg3)
{
    register ULONG r0 RK_ASM("r0") = callNumber;
    register ULONG r1 RK_ASM("r1") = arg0;
    register ULONG r2 RK_ASM("r2") = arg1;
    register ULONG r3 RK_ASM("r3") = arg2;
    register ULONG r12 RK_ASM("r12") = arg3;

    RK_ASM volatile("SVC #0"
                    : "+r"(r0)
                    : "r"(r1), "r"(r2), "r"(r3), "r"(r12)
                    : "memory");

    return (r0);
}

RK_FORCE_INLINE static inline unsigned kIsISR(void)
{
    unsigned ipsr_value;
    RK_ASM("MRS %0, IPSR" : "=r"(ipsr_value));
    if (ipsr_value == 0U)
    {
        return (0U);
    }

    if ((RK_gSyscallThreadModeActive != 0U) &&
        (ipsr_value == RK_CORE_SVC_EXCEPTION))
    {
        return (0U);
    }

    return (ipsr_value);
}

RK_FORCE_INLINE
static inline unsigned __getReadyPrio(unsigned mask)
{
    unsigned result;
    RK_ASM volatile(
        "clz   %[out], %[in]      \n"
        "neg   %[out], %[out]     \n"
        "add   %[out], %[out], #31\n" : [out] "=&r"(result) : [in] "r"(mask) :);
    return result;
}

RK_FORCE_INLINE
static inline RK_ERR kInitStack_(UINT *const stackBufPtr, UINT const stackSize,
                                 RK_TASKENTRY const taskFunc, VOID *argsPtr)
{
    if (stackBufPtr == NULL || stackSize < RK_MIN_STACKSIZE || taskFunc == NULL)
    {
        return (RK_ERR_ERROR);
    }

    stackBufPtr[stackSize - PSR_OFFSET] = 0x01000000U;
    stackBufPtr[stackSize - PC_OFFSET] = (UINT)taskFunc;
    stackBufPtr[stackSize - LR_OFFSET] = 0x14141414U;
    stackBufPtr[stackSize - R12_OFFSET] = 0x12121212U;
    stackBufPtr[stackSize - R3_OFFSET] = 0x03030303U;
    stackBufPtr[stackSize - R2_OFFSET] = 0x02020202U;
    stackBufPtr[stackSize - R1_OFFSET] = 0x01010101U;
    stackBufPtr[stackSize - R0_OFFSET] = (UINT)(argsPtr);
    stackBufPtr[stackSize - R11_OFFSET] = 0x11111111U;
    stackBufPtr[stackSize - R10_OFFSET] = 0x10101010U;
    stackBufPtr[stackSize - R9_OFFSET] = 0x09090909U;
    stackBufPtr[stackSize - R8_OFFSET] = 0x08080808U;
    stackBufPtr[stackSize - R7_OFFSET] = 0x07070707U;
    stackBufPtr[stackSize - R6_OFFSET] = 0x06060606U;
    stackBufPtr[stackSize - R5_OFFSET] = 0x05050505U;
    stackBufPtr[stackSize - R4_OFFSET] = 0x04040404U;

    /* stack painting */
    for (ULONG j = 17U; j < stackSize; j++)
    {
        stackBufPtr[stackSize - j] = RK_STACK_PATTERN;
    }
    stackBufPtr[0] = RK_STACK_GUARD;
    return (RK_ERR_SUCCESS);
}

#ifdef __cplusplus
}
#endif

#endif /* RK_COREDEFS_H */
