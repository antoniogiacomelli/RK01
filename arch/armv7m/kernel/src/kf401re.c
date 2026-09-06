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
 *   STM32F401RE hardware abstraction helpers that do not belong to the generic
 *   ARMv7-M core. Keep board peripheral register details behind this boundary
 *   so application code can depend on khal.h instead of raw STM32 registers.
 */

#define RK_SOURCE_CODE

#include <kf401re.h>

#if defined(RK_MCU_F401RE)

#define K_F401RE_IWDG_LSI_HZ (32000UL)
#define K_F401RE_IWDG_TIMEOUT_MS (8000UL)
#define K_F401RE_IWDG_WAIT_LIMIT (1000000UL)
#define K_F401RE_IWDG_RELOAD \
    (((K_F401RE_IWDG_TIMEOUT_MS * (K_F401RE_IWDG_LSI_HZ / 1000UL)) / \
      K_F401RE_IWDG_PRESCALER_DIVIDER) - 1UL)

#if (K_F401RE_IWDG_RELOAD > K_F401RE_IWDG_RELOAD_MAX)
#error "STM32F401RE watchdog reload exceeds IWDG_RLR width"
#endif

static RK_BOOL kF401reWatchdogClockEnable_(VOID)
{
    ULONG wait = K_F401RE_IWDG_WAIT_LIMIT;

    K_F401RE_RCC_CSR |= K_F401RE_RCC_CSR_LSION;
    while ((K_F401RE_RCC_CSR & K_F401RE_RCC_CSR_LSIRDY) == 0UL)
    {
        if (wait == 0UL)
        {
            return (RK_FALSE);
        }

        wait--;
    }

    return (RK_TRUE);
}

static RK_BOOL kF401reWatchdogWaitConfig_(VOID)
{
    ULONG wait = K_F401RE_IWDG_WAIT_LIMIT;

    while ((K_F401RE_IWDG_SR & K_F401RE_IWDG_SR_UPDATE_BUSY) != 0UL)
    {
        if (wait == 0UL)
        {
            return (RK_FALSE);
        }

        wait--;
    }

    return (RK_TRUE);
}

static VOID kF401reWatchdogDebugFreeze_(VOID)
{
#if !defined(NDEBUG)
    K_F401RE_DBGMCU_APB1_FZ |= K_F401RE_DBGMCU_APB1_FZ_DBG_IWDG_STOP;
#endif
}

VOID kF401reWatchdogConfigure(VOID)
{
    if (kF401reWatchdogClockEnable_() == RK_FALSE)
    {
        return;
    }

    kF401reWatchdogDebugFreeze_();

    K_F401RE_IWDG_KR = K_F401RE_IWDG_KEY_START;
    K_F401RE_IWDG_KR = K_F401RE_IWDG_KEY_UNLOCK;
    if (kF401reWatchdogWaitConfig_() == RK_FALSE)
    {
        return;
    }

    K_F401RE_IWDG_PR = K_F401RE_IWDG_PRESCALER_DIV256;
    K_F401RE_IWDG_RLR = K_F401RE_IWDG_RELOAD;
    if (kF401reWatchdogWaitConfig_() == RK_FALSE)
    {
        return;
    }

    kF401reWatchdogKick();
}

VOID kF401reWatchdogKick(VOID)
{
    K_F401RE_IWDG_KR = K_F401RE_IWDG_KEY_RELOAD;
}

#else

VOID kF401reWatchdogConfigure(VOID)
{
}

VOID kF401reWatchdogKick(VOID)
{
}

#endif
