/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_HAL_H
#define RK_HAL_H

#include <kcommondefs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generic Cortex-M memory-mapped register helpers. */
#ifndef RK_HW_REG
#define RK_HW_REG(addr) (*(volatile ULONG *)(addr))
#endif

/* ARMv7-M/ARMv8-M System Control Space base addresses used by RK01. */
#define RK_CORE_SCB_BASE (0xE000ED00UL)
#define RK_CORE_SYSTICK_BASE (0xE000E010UL)
#define RK_CORE_NVIC_BASE (0xE000E100UL)
#define RK_CORE_NVIC_IPR_BASE (0xE000E400UL)
#define RK_CORE_MPU_BASE (0xE000ED90UL)
#define RK_CORE_FPU_BASE (0xE000EF30UL)

/* System Control Block registers. */
#define RK_REG_SCB_ICSR RK_HW_REG(RK_CORE_SCB_BASE + 0x04UL)
#define RK_REG_SCB_CPACR RK_HW_REG(RK_CORE_SCB_BASE + 0x88UL)
#define RK_REG_SCB_SHCSR RK_HW_REG(RK_CORE_SCB_BASE + 0x24UL)
#define RK_REG_SCB_CFSR RK_HW_REG(RK_CORE_SCB_BASE + 0x28UL)
#define RK_REG_SCB_MMFAR RK_HW_REG(RK_CORE_SCB_BASE + 0x34UL)
#define RK_CORE_SCB_SHPR_BASE (RK_CORE_SCB_BASE + 0x18UL)

/* SysTick registers. */
#define RK_REG_SYSTICK_CTRL RK_HW_REG(RK_CORE_SYSTICK_BASE + 0x00UL)
#define RK_REG_SYSTICK_LOAD RK_HW_REG(RK_CORE_SYSTICK_BASE + 0x04UL)
#define RK_REG_SYSTICK_VAL RK_HW_REG(RK_CORE_SYSTICK_BASE + 0x08UL)

/* NVIC register window base. Callers add the peripheral-specific offset. */
#define RK_REG_NVIC RK_HW_REG(RK_CORE_NVIC_BASE)

/* MPU registers. RASR and RLAR intentionally share the same offset on
 * different Cortex-M MPU generations. */
#define RK_REG_MPU_TYPE RK_HW_REG(RK_CORE_MPU_BASE + 0x00UL)
#define RK_REG_MPU_CTRL RK_HW_REG(RK_CORE_MPU_BASE + 0x04UL)
#define RK_REG_MPU_RNR RK_HW_REG(RK_CORE_MPU_BASE + 0x08UL)
#define RK_REG_MPU_RBAR RK_HW_REG(RK_CORE_MPU_BASE + 0x0CUL)
#define RK_REG_MPU_RASR RK_HW_REG(RK_CORE_MPU_BASE + 0x10UL)
#define RK_REG_MPU_RLAR RK_HW_REG(RK_CORE_MPU_BASE + 0x10UL)
#define RK_REG_MPU_MAIR0 RK_HW_REG(RK_CORE_MPU_BASE + 0x30UL)
#define RK_REG_MPU_MAIR1 RK_HW_REG(RK_CORE_MPU_BASE + 0x34UL)

/* FPU control registers. */
#define RK_REG_FPU_FPCCR RK_HW_REG(RK_CORE_FPU_BASE + 0x04UL)
#define RK_SCB_CPACR_CP10_CP11_FULL_ACCESS (0xFUL << 20U)
#define RK_FPU_FPCCR_ASPEN (1UL << 31U)
#define RK_FPU_FPCCR_LSPEN (1UL << 30U)

#if defined(RK_MCU_F401RE)
#include <kf401re.h>
#define K_HAL_HAS_WATCHDOG (1U)
#define K_HAL_WATCHDOG_FEED_MS K_F401RE_WATCHDOG_FEED_MS

static inline VOID kHalWatchdogConfigure(VOID)
{
    kF401reWatchdogConfigure();
}

static inline VOID kHalWatchdogKick(VOID)
{
    kF401reWatchdogKick();
}
#else
#define K_HAL_HAS_WATCHDOG (0U)
#define K_HAL_WATCHDOG_FEED_MS (0UL)

static inline VOID kHalWatchdogConfigure(VOID)
{
}

static inline VOID kHalWatchdogKick(VOID)
{
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* RK_HAL_H */
