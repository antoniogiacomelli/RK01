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
 *   ARMv7-M core and board bring-up. This file owns clock setup, exception
 *   priorities, SysTick programming, critical sections and reset helpers.
 *
 * Contracts/invariants:
 *   - SVC runs above SysTick in the production ordering.
 *   - SysTick runs above PendSV, and PendSV remains the lowest-priority
 *     context-switch exception.
 *   - Priority writes preserve neighboring byte fields in SCB/NVIC registers.
 */

#include <kcoredefs.h>
#include <kmpu.h>
#if defined(__GNUC__)
#define RK_WEAK __attribute__((weak))
#else
#define RK_WEAK
#endif

#if (RK_CONF_SYSCORECLK == 0)
/* CMSIS-Core may export a strong SystemCoreClock symbol. */
unsigned long int SystemCoreClock RK_WEAK = RK_CONF_EFFECTIVE_SYSCORECLK;
unsigned long RK_gSysCoreClock = 0UL;
#else
unsigned long int SystemCoreClock RK_WEAK = RK_CONF_SYSCORECLK;
unsigned long RK_gSysCoreClock = RK_CONF_SYSCORECLK;
#endif

#if defined(STM32F401xE) && (RK_CONF_EFFECTIVE_SYSCORECLK == 80000000UL)
static VOID kCoreClockInit_(VOID)
{
    K_F401RE_RCC_CR |= K_F401RE_RCC_CR_HSION;
    while ((K_F401RE_RCC_CR & K_F401RE_RCC_CR_HSIRDY) == 0UL)
    {
    }

    K_F401RE_FLASH_ACR =
        (K_F401RE_FLASH_ACR & ~K_F401RE_FLASH_ACR_LATENCY_MASK) |
        K_F401RE_FLASH_ACR_LATENCY_2WS | K_F401RE_FLASH_ACR_PRFTEN |
        K_F401RE_FLASH_ACR_ICEN | K_F401RE_FLASH_ACR_DCEN;

    if ((K_F401RE_RCC_CFGR & K_F401RE_RCC_CFGR_SWS_MASK) ==
        K_F401RE_RCC_CFGR_SWS_PLL)
    {
        K_F401RE_RCC_CFGR =
            (K_F401RE_RCC_CFGR & ~K_F401RE_RCC_CFGR_SW_MASK) |
            K_F401RE_RCC_CFGR_SW_HSI;
        while ((K_F401RE_RCC_CFGR & K_F401RE_RCC_CFGR_SWS_MASK) !=
               K_F401RE_RCC_CFGR_SWS_HSI)
        {
        }
    }

    K_F401RE_RCC_CR &= ~K_F401RE_RCC_CR_PLLON;
    while ((K_F401RE_RCC_CR & K_F401RE_RCC_CR_PLLRDY) != 0UL)
    {
    }

    K_F401RE_RCC_PLLCFGR = K_F401RE_RCC_PLLCFGR_PLLM(16UL) |
                           K_F401RE_RCC_PLLCFGR_PLLN(320UL) |
                           K_F401RE_RCC_PLLCFGR_PLLP_DIV4 |
                           K_F401RE_RCC_PLLCFGR_PLLQ(7UL);

    K_F401RE_RCC_CFGR =
        (K_F401RE_RCC_CFGR &
         ~(K_F401RE_RCC_CFGR_HPRE_MASK | K_F401RE_RCC_CFGR_PPRE1_MASK |
           K_F401RE_RCC_CFGR_PPRE2_MASK)) |
        K_F401RE_RCC_CFGR_PPRE1_DIV2;

    K_F401RE_RCC_CR |= K_F401RE_RCC_CR_PLLON;
    while ((K_F401RE_RCC_CR & K_F401RE_RCC_CR_PLLRDY) == 0UL)
    {
    }

    K_F401RE_RCC_CFGR =
        (K_F401RE_RCC_CFGR & ~K_F401RE_RCC_CFGR_SW_MASK) |
        K_F401RE_RCC_CFGR_SW_PLL;
    while ((K_F401RE_RCC_CFGR & K_F401RE_RCC_CFGR_SWS_MASK) !=
           K_F401RE_RCC_CFGR_SWS_PLL)
    {
    }

    SystemCoreClock = RK_CONF_EFFECTIVE_SYSCORECLK;
    RK_gSysCoreClock = RK_CONF_EFFECTIVE_SYSCORECLK;
}
#else
static VOID kCoreClockInit_(VOID)
{
}
#endif

static VOID kCoreResolveClock_(VOID)
{
    if (RK_gSysCoreClock == 0UL)
    {
        RK_gSysCoreClock = SystemCoreClock;
    }

#if (RK_CONF_EFFECTIVE_SYSCORECLK > 0UL)
    if (RK_gSysCoreClock == 0UL)
    {
        RK_gSysCoreClock = RK_CONF_EFFECTIVE_SYSCORECLK;
    }
#endif

    SystemCoreClock = RK_gSysCoreClock;
}

static inline unsigned kCoreSysTickConfig_(unsigned ticks)
{
    /* SysTick LOAD is 24 bits; reject values the hardware cannot represent. */
    if ((ticks - 1) > 0xFFFFFFUL) /*24-bit max*/
    {
        return (0xFFFFFFFF);
    }
    kCoreResolveClock_();

    /* Set reload register */
    RK_REG_SYSTICK_LOAD = (ticks - 1);

    /* Reset the SysTick counter */
    RK_REG_SYSTICK_VAL = 0;

    RK_REG_SYSTICK_CTRL = 0x06; /* keep interrupt disabled */

#ifndef RK_CONF_SYSTICK_DIV

    RK_gSysTickInterval = (ticks * 1000UL) / (RK_gSysCoreClock);

#else

    RK_gSysTickInterval = 1000UL / RK_CONF_SYSTICK_DIV;

#endif

    return (0);
}

#define RK_CORE_PRIO_SHIFT (8U - RK_CONF_NPRIO_BITS)
#define RK_CORE_LOWEST_PRIO ((1U << RK_CONF_NPRIO_BITS) - 1U)
#define RK_CORE_PENDSV_PRIO RK_CORE_LOWEST_PRIO

#if (RK_CONF_SYSTICK_ABOVE_SVC == ON)
#define RK_CORE_SYSTICK_PRIO (RK_CORE_LOWEST_PRIO - 2U)
#define RK_CORE_SVC_PRIO (RK_CORE_LOWEST_PRIO - 1U)
#else
#define RK_CORE_SVC_PRIO (RK_CORE_LOWEST_PRIO - 2U)
#define RK_CORE_SYSTICK_PRIO (RK_CORE_LOWEST_PRIO - 1U)
#endif

/*
 * ARMv7-M stores interrupt priorities in the upper implemented bits of each
 * byte-wide priority field. These macros derive a portable ordering from the
 * number of implemented priority bits instead of assuming a fixed 0..7 range.
 */
#define RK_CORE_STATIC_ASSERT_(COND, NAME) typedef char NAME[(COND) ? 1 : -1]

RK_CORE_STATIC_ASSERT_(RK_CONF_NPRIO_BITS > 1U,
                       rk_core_needs_at_least_two_prio_bits_);
RK_CORE_STATIC_ASSERT_(RK_CORE_PENDSV_PRIO == RK_CORE_LOWEST_PRIO,
                       rk_core_pendsv_must_be_lowest_prio_);
#if (RK_CONF_SYSTICK_ABOVE_SVC != ON)
RK_CORE_STATIC_ASSERT_(RK_CORE_SVC_PRIO < RK_CORE_SYSTICK_PRIO,
                       rk_core_svc_must_run_above_systick_);
#endif
RK_CORE_STATIC_ASSERT_(RK_CORE_SYSTICK_PRIO < RK_CORE_PENDSV_PRIO,
                       rk_core_systick_must_run_above_pendsv_);

/*
 * Write one byte-wide priority field inside SCB_SHPR or NVIC_IPR. The mask
 * preserves neighboring priority fields that share the same 32-bit register.
 */
static inline VOID kCoreWritePriorityField_(ULONG const baseAddress,
                                            ULONG const fieldOffset,
                                            unsigned const priority)
{
    ULONG const regAddress = baseAddress + (fieldOffset & ~3UL);
    ULONG const shift = (fieldOffset & 3UL) * 8UL;
    ULONG const mask = 0xFFUL << shift;
    ULONG const value =
        ((ULONG)((priority << RK_CORE_PRIO_SHIFT) & 0xFFU)) << shift;

    volatile ULONG *const regPtr = (volatile ULONG *)regAddress;
    *regPtr = (*regPtr & ~mask) | value;
}

static inline
void kCoreSetInterruptPriority_(int IRQn, unsigned priority)
{
    if (IRQn < 0)
    {
        unsigned long offset = ((unsigned long)IRQn & 0xFUL) - 4UL;
        /* System handler priority */
        kCoreWritePriorityField_(RK_CORE_SCB_SHPR_BASE, offset, priority);
    }
    else
    {
        /* IRQ priority */
        kCoreWritePriorityField_(RK_CORE_NVIC_IPR_BASE, (ULONG)IRQn,
                                 priority);
    }
}


void kCoreInit(void)
{

    kCoreClockInit_();
    kCoreResolveClock_();

    kCoreSysTickConfig_(RK_gSysCoreClock / RK_CONF_SYSTICK_DIV);
#if (RK_CONF_SYSTICK_ABOVE_SVC == ON)
    /* Test mode: let SysTick interrupt SVC to exercise deferred tick draining. */
    kCoreSetInterruptPriority_(RK_CORE_SYSTICK_IRQN, RK_CORE_SYSTICK_PRIO);
    kCoreSetInterruptPriority_(RK_CORE_SVC_IRQN, RK_CORE_SVC_PRIO);
#else
    /* RTX-style ordering: SVC runs above SysTick/PendSV, and PendSV is last. */
    kCoreSetInterruptPriority_(RK_CORE_SVC_IRQN, RK_CORE_SVC_PRIO);
    kCoreSetInterruptPriority_(RK_CORE_SYSTICK_IRQN, RK_CORE_SYSTICK_PRIO);
#endif
    kCoreSetInterruptPriority_(RK_CORE_PENDSV_IRQN, RK_CORE_PENDSV_PRIO);
    kMpuInit();
}
