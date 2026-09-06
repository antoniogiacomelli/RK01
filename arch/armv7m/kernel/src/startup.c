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
 *   Reset and exception-vector entry point. Startup copies initialised data,
 *   clears BSS-style regions, prepares the C runtime and routes hardware
 *   exceptions into the kernel fault path.
 */

#pragma GCC diagnostic ignored "-Wpedantic"
/* see comment on vector table definition */

#include <stdint.h>
#include <khal.h>

#if defined(__FPU_PRESENT) && (__FPU_PRESENT == 1)
static void kStartupFpuInit_(void)
{
    /*
     * Enable CP10/CP11 before any floating-point instruction can execute.
     * ASPEN keeps hardware FP context tracking active; LSPEN is cleared so
     * exception entry pays a known cost instead of a later lazy-stacking cost.
     */
    RK_REG_SCB_CPACR |= RK_SCB_CPACR_CP10_CP11_FULL_ACCESS;
    __asm volatile("DSB\n"
                   "ISB" ::: "memory");

    RK_REG_FPU_FPCCR =
        (RK_REG_FPU_FPCCR | RK_FPU_FPCCR_ASPEN) & ~RK_FPU_FPCCR_LSPEN;
    __asm volatile("DSB\n"
                   "ISB" ::: "memory");
}
#endif

/* Forward declaration of the system exception handlers */
void Reset_Handler(void);
void Default_Handler(void) __attribute__((noreturn));
void NMI_Handler(void)
    __attribute__((weak, alias("Default_Handler"), noreturn));
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

/* Forward declaration of standard peripheral interrupt handlers */
void GPIO_Handler(void)
    __attribute__((weak, alias("Default_Handler"), noreturn));
void UART0_Handler(void)
    __attribute__((weak, alias("Default_Handler"), noreturn));
void UART1_Handler(void)
    __attribute__((weak, alias("Default_Handler"), noreturn));
void USART2_IRQHandler(void)
    __attribute__((weak, alias("Default_Handler"), noreturn));
void SSI_Handler(void)
    __attribute__((weak, alias("Default_Handler"), noreturn));
void I2C_Handler(void)
    __attribute__((weak, alias("Default_Handler"), noreturn));
void PWM_Handler(void)
    __attribute__((weak, alias("Default_Handler"), noreturn));
void ADC_Handler(void)
    __attribute__((weak, alias("Default_Handler"), noreturn));
/* External definitions */
extern uint32_t _sidata;     /* Start address of the initialisation values of the .data section */
extern uint32_t _sdata;      /* Start address of the .data section */
extern uint32_t _edata;      /* End address of the .data section */
extern uint32_t _sbss;       /* Start address of the .bss section */
extern uint32_t _ebss;       /* End address of the .bss section */
extern uint32_t __rk_app_domain_data_load;
extern uint32_t __rk_app_domain_data_begin;
extern uint32_t __rk_app_domain_data_end;
extern uint32_t __rk_domain_bss_begin;
extern uint32_t __rk_domain_bss_end;
extern uint32_t __rk_app_domain_bss_begin;
extern uint32_t __rk_app_domain_bss_end;
extern uint32_t __rk_shared_bss_begin;
extern uint32_t __rk_shared_bss_end;
extern uint32_t _estack;     /* alias for __stack */
/* Forward declaration for the main function */
extern int main(void);

volatile uint32_t RK_gDefaultHandlerVector;
volatile uint32_t RK_gDefaultHandlerIcsr;
volatile uint32_t RK_gDefaultHandlerCfsr;
volatile uint32_t RK_gDefaultHandlerHfsr;
volatile uint32_t RK_gDefaultHandlerShcsr;
volatile uint32_t RK_gDefaultHandlerMmfar;
volatile uint32_t RK_gDefaultHandlerBfar;
volatile uint32_t RK_gDefaultHandlerExcReturn;
volatile uint32_t RK_gDefaultHandlerFramePtr;
volatile uint32_t RK_gDefaultHandlerStackedR0;
volatile uint32_t RK_gDefaultHandlerStackedR1;
volatile uint32_t RK_gDefaultHandlerStackedR2;
volatile uint32_t RK_gDefaultHandlerStackedR3;
volatile uint32_t RK_gDefaultHandlerStackedR12;
volatile uint32_t RK_gDefaultHandlerStackedLr;
volatile uint32_t RK_gDefaultHandlerStackedPc;
volatile uint32_t RK_gDefaultHandlerStackedXpsr;

#define RK_STARTUP_STR_(x) #x
#define RK_STARTUP_STR(x) RK_STARTUP_STR_(x)

#if defined(__FPU_PRESENT) && (__FPU_PRESENT == 1)
#define RK_CAPTURE_FAULT_CORE_FRAME_ASM                                        \
    "TST LR, #0x10\n"                                                          \
    "IT EQ\n"                                                                  \
    "ADDEQ R0, R0, #72\n"
#else
#define RK_CAPTURE_FAULT_CORE_FRAME_ASM
#endif

#define RK_CAPTURE_FAULT_FRAME_ASM                                             \
    "TST LR, #4\n"                                                             \
    "ITE EQ\n"                                                                 \
    "MRSEQ R0, MSP\n"                                                          \
    "MRSNE R0, PSP\n"                                                          \
    RK_CAPTURE_FAULT_CORE_FRAME_ASM                                            \
    "LDR R3, =RK_gDefaultHandlerExcReturn\n"                                  \
    "STR LR, [R3]\n"                                                           \
    "LDR R3, =RK_gDefaultHandlerFramePtr\n"                                   \
    "STR R0, [R3]\n"

#define RK_CAPTURE_FAULT_SCB_REST_ASM                                          \
    "LDR R3, =0xE000ED24\n"                                                    \
    "LDR R2, [R3]\n"                                                           \
    "LDR R3, =RK_gDefaultHandlerShcsr\n"                                      \
    "STR R2, [R3]\n"                                                           \
    "LDR R3, =0xE000ED28\n"                                                    \
    "LDR R2, [R3]\n"                                                           \
    "LDR R3, =RK_gDefaultHandlerCfsr\n"                                       \
    "STR R2, [R3]\n"                                                           \
    "LDR R3, =0xE000ED2C\n"                                                    \
    "LDR R2, [R3]\n"                                                           \
    "LDR R3, =RK_gDefaultHandlerHfsr\n"                                       \
    "STR R2, [R3]\n"                                                           \
    "LDR R3, =0xE000ED34\n"                                                    \
    "LDR R2, [R3]\n"                                                           \
    "LDR R3, =RK_gDefaultHandlerMmfar\n"                                      \
    "STR R2, [R3]\n"                                                           \
    "LDR R3, =0xE000ED38\n"                                                    \
    "LDR R2, [R3]\n"                                                           \
    "LDR R3, =RK_gDefaultHandlerBfar\n"                                       \
    "STR R2, [R3]\n"

#define RK_CAPTURE_FAULT_STACKED_ASM                                           \
    "CMP R0, #0\n"                                                             \
    "BEQ 1f\n"                                                                 \
    "LDR R2, [R0, #0]\n"                                                       \
    "LDR R3, =RK_gDefaultHandlerStackedR0\n"                                  \
    "STR R2, [R3]\n"                                                           \
    "LDR R2, [R0, #4]\n"                                                       \
    "LDR R3, =RK_gDefaultHandlerStackedR1\n"                                  \
    "STR R2, [R3]\n"                                                           \
    "LDR R2, [R0, #8]\n"                                                       \
    "LDR R3, =RK_gDefaultHandlerStackedR2\n"                                  \
    "STR R2, [R3]\n"                                                           \
    "LDR R2, [R0, #12]\n"                                                      \
    "LDR R3, =RK_gDefaultHandlerStackedR3\n"                                  \
    "STR R2, [R3]\n"                                                           \
    "LDR R2, [R0, #16]\n"                                                      \
    "LDR R3, =RK_gDefaultHandlerStackedR12\n"                                 \
    "STR R2, [R3]\n"                                                           \
    "LDR R2, [R0, #20]\n"                                                      \
    "LDR R3, =RK_gDefaultHandlerStackedLr\n"                                  \
    "STR R2, [R3]\n"                                                           \
    "LDR R2, [R0, #24]\n"                                                      \
    "LDR R3, =RK_gDefaultHandlerStackedPc\n"                                  \
    "STR R2, [R3]\n"                                                           \
    "LDR R2, [R0, #28]\n"                                                      \
    "LDR R3, =RK_gDefaultHandlerStackedXpsr\n"                                \
    "STR R2, [R3]\n"                                                           \
    "1:\n"

#define RK_FAULT_SPIN_ASM                                                      \
    "2:\n"                                                                     \
    "NOP\n"                                                                    \
    "B 2b\n"

#define RK_CAPTURE_FIXED_FAULT_ASM(vector_)                                    \
    RK_CAPTURE_FAULT_FRAME_ASM                                                 \
    "MOVS R2, #" RK_STARTUP_STR(vector_) "\n"                                  \
    "LDR R3, =RK_gDefaultHandlerVector\n"                                     \
    "STR R2, [R3]\n"                                                           \
    "LDR R3, =0xE000ED04\n"                                                    \
    "LDR R2, [R3]\n"                                                           \
    "LDR R3, =RK_gDefaultHandlerIcsr\n"                                       \
    "STR R2, [R3]\n"                                                           \
    RK_CAPTURE_FAULT_SCB_REST_ASM                                              \
    RK_CAPTURE_FAULT_STACKED_ASM                                               \
    RK_FAULT_SPIN_ASM

#define RK_CAPTURE_ACTIVE_FAULT_ASM                                            \
    RK_CAPTURE_FAULT_FRAME_ASM                                                 \
    "LDR R3, =0xE000ED04\n"                                                    \
    "LDR R2, [R3]\n"                                                           \
    "LDR R3, =RK_gDefaultHandlerIcsr\n"                                       \
    "STR R2, [R3]\n"                                                           \
    "LDR R3, =0x1FF\n"                                                         \
    "ANDS R2, R2, R3\n"                                                        \
    "LDR R3, =RK_gDefaultHandlerVector\n"                                     \
    "STR R2, [R3]\n"                                                           \
    RK_CAPTURE_FAULT_SCB_REST_ASM                                              \
    RK_CAPTURE_FAULT_STACKED_ASM                                               \
    RK_FAULT_SPIN_ASM

/* The Vector Table */
__attribute__ ((section(".isr_vector")))
void (* const g_pfnVectors[])(void) =
{
    /* Core system exceptions */
   /* pedantic warning is ignored on start-up because of this cast of a function pointer to generic pointer ; workarounds are too cumbersome */
   (void*)&_estack,         /* The initial stack pointer */
    Reset_Handler,               /* The reset handler */
    NMI_Handler,                 /* The NMI handler */
    HardFault_Handler,           /* The hard fault handler */
    MemManage_Handler,           /* The MPU fault handler */
    BusFault_Handler,            /* The bus fault handler */
    UsageFault_Handler,          /* The usage fault handler */
    0,                           /* Reserved */
    0,                           /* Reserved */
    0,                           /* Reserved */
    0,                           /* Reserved */
    SVC_Handler,                 /* SVCall handler */
    DebugMon_Handler,            /* Debug monitor handler */
    0,                           /* Reserved */
    PendSV_Handler,              /* The PendSV handler */
    SysTick_Handler,             /* The SysTick handler */

    /* External interrupts */
    GPIO_Handler,                /* IRQ 0: GPIO */
    Default_Handler,             /* IRQ 1 */
    Default_Handler,             /* IRQ 2 */
    Default_Handler,             /* IRQ 3 */
    Default_Handler,             /* IRQ 4 */
    UART0_Handler,               /* IRQ 5: UART0 */
    UART1_Handler,               /* IRQ 6: UART1 */
    SSI_Handler,                 /* IRQ 7: SSI */
    I2C_Handler,                 /* IRQ 8: I2C */
    PWM_Handler,                 /* IRQ 9: PWM */
    Default_Handler,             /* IRQ 10 */
    Default_Handler,             /* IRQ 11 */
    Default_Handler,             /* IRQ 12 */
    Default_Handler,             /* IRQ 13 */
    ADC_Handler,                 /* IRQ 14: ADC */
    Default_Handler,             /* IRQ 15 */
    Default_Handler,             /* IRQ 16 */
    Default_Handler,             /* IRQ 17 */
    Default_Handler,             /* IRQ 18 */
    Default_Handler,             /* IRQ 19 */
    Default_Handler,             /* IRQ 20 */
    Default_Handler,             /* IRQ 21 */
    Default_Handler,             /* IRQ 22 */
    Default_Handler,             /* IRQ 23 */
    Default_Handler,             /* IRQ 24 */
    Default_Handler,             /* IRQ 25 */
    Default_Handler,             /* IRQ 26 */
    Default_Handler,             /* IRQ 27 */
    Default_Handler,             /* IRQ 28 */
    Default_Handler,             /* IRQ 29 */
    Default_Handler,             /* IRQ 30 */
    Default_Handler,             /* IRQ 31 */
    Default_Handler,             /* IRQ 32 */
    Default_Handler,             /* IRQ 33 */
    Default_Handler,             /* IRQ 34 */
    Default_Handler,             /* IRQ 35 */
    Default_Handler,             /* IRQ 36 */
    Default_Handler,             /* IRQ 37 */
    USART2_IRQHandler,           /* IRQ 38: STM32F401RE USART2 */
};


/**
 * @brief  System initialisation function
 */
void SystemInit(void) {
#if defined(__FPU_PRESENT) && (__FPU_PRESENT == 1)
    kStartupFpuInit_();
#endif
    /* RK01 will handle the rest of system initialisation in kCoreInit(). */
}

/**
 * @brief  Reset handler
 */
void Reset_Handler(void)
{
    uint32_t const *pSrc;
    uint32_t *pDest;

    /* Copy the data segment initialisers from flash to SRAM */
    pSrc = &_sidata;
    pDest = &_sdata;

    /* cppcheck-suppress comparePointers */
    while (pDest < &_edata)
    {
        *pDest++ = *pSrc++;
    }

    /* Copy application initialised globals into the App domain RAM window. */
    pSrc = &__rk_app_domain_data_load;
    pDest = &__rk_app_domain_data_begin;

    /* cppcheck-suppress comparePointers */
    while (pDest < &__rk_app_domain_data_end)
    {
        *pDest++ = *pSrc++;
    }

    /* Zero fill the bss segment */
    /* cppcheck-suppress comparePointers */
    for (pDest = &_sbss; pDest < &_ebss; pDest++)
    {
        *pDest = 0;
    }

    /* Zero user/domain and shared BSS sections that are deliberately NOLOAD. */
    for (pDest = &__rk_app_domain_bss_begin;
         pDest < &__rk_app_domain_bss_end; pDest++)
    {
        *pDest = 0;
    }

    for (pDest = &__rk_domain_bss_begin; pDest < &__rk_domain_bss_end; pDest++)
    {
        *pDest = 0;
    }

    for (pDest = &__rk_shared_bss_begin; pDest < &__rk_shared_bss_end; pDest++)
    {
        *pDest = 0;
    }

    /* Call system initialisation function */
    SystemInit();

    /* Call the application's entry point */
    main();

    /* Loop forever if main() returns */
    while (1);
}

__attribute__((naked, noreturn))
void HardFault_Handler(void)
{
    __asm volatile(RK_CAPTURE_FIXED_FAULT_ASM(3));
}

__attribute__((naked, noreturn))
void BusFault_Handler(void)
{
    __asm volatile(RK_CAPTURE_FIXED_FAULT_ASM(5));
}

__attribute__((naked, noreturn))
void UsageFault_Handler(void)
{
    __asm volatile(RK_CAPTURE_FIXED_FAULT_ASM(6));
}

/**
 * @brief  This is the code that gets called when the processor receives an
 *         unexpected interrupt.
 */
__attribute__((naked, noreturn))
void Default_Handler(void)
{
    __asm volatile(RK_CAPTURE_ACTIVE_FAULT_ASM);
}

void DebugMon_Handler(void)
{
    while(1)
    ;
}
