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
 *   STM32F401RE board-console backend. The kernel uses this small USART2 path
 *   for privileged diagnostics, panic output, logger draining and optional RX
 *   interrupt delivery.
 */

#define RK_SOURCE_CODE

#include <kconsole.h>
#include <kconfig.h>
#include <kcoredefs.h>
#include <ksyscall.h>

#if defined(STM32F401xE)
#define RK_BOARD_CONSOLE_HAS_USART2 (1U)
#define RK_BOARD_CONSOLE_BAUD (115200UL)
#endif

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
#define RK_BOARD_CONSOLE_IRQ_LOWEST_PRIO ((1U << RK_CONF_NPRIO_BITS) - 1U)
#define RK_BOARD_CONSOLE_IRQ_PRIO_SHIFT (8U - RK_CONF_NPRIO_BITS)

static RK_CONSOLE_RX_ISR_CBK kBoardConsoleRxCbk_;

static void kBoardConsoleNvicPrioritySet_(ULONG const irqn,
                                          UINT const priority)
{
    ULONG const regAddress = RK_CORE_NVIC_IPR_BASE + (irqn & ~3UL);
    ULONG const shift = (irqn & 3UL) * 8UL;
    ULONG const mask = 0xFFUL << shift;
    ULONG const value =
        ((ULONG)((priority << RK_BOARD_CONSOLE_IRQ_PRIO_SHIFT) & 0xFFU)) <<
        shift;
    volatile ULONG *const regPtr = (volatile ULONG *)regAddress;

    *regPtr = (*regPtr & ~mask) | value;
}

static void kBoardConsoleNvicEnable_(ULONG const irqn)
{
    volatile ULONG *const regPtr =
        (volatile ULONG *)(RK_CORE_NVIC_BASE + ((irqn / 32UL) * 4UL));

    *regPtr = 1UL << (irqn & 31UL);
}
#endif

#if defined(STM32F401xE)
/*
 * Nucleo-F401RE exposes the ST-LINK virtual COM port through USART2:
 * PA2 = USART2_TX, PA3 = USART2_RX, both using alternate function AF7.
 */
static void kBoardConsolePinsInit_(void)
{
    volatile unsigned long fence;

    /* Enable peripheral clocks before touching GPIOA or USART2 registers. */
    K_F401RE_RCC_AHB1ENR |= K_F401RE_RCC_AHB1ENR_GPIOAEN;
    K_F401RE_RCC_APB1ENR |= K_F401RE_RCC_APB1ENR_USART2EN;

    fence = K_F401RE_RCC_AHB1ENR;
    fence = K_F401RE_RCC_APB1ENR;
    (void)fence;

    /* Put PA2/PA3 in AF7 mode with push-pull output and a pull-up on RX. */
    K_F401RE_GPIOA_MODER &= ~((3UL << (2U * 2U)) | (3UL << (3U * 2U)));
    K_F401RE_GPIOA_MODER |= ((2UL << (2U * 2U)) | (2UL << (3U * 2U)));
    K_F401RE_GPIOA_OTYPER &= ~((1UL << 2U) | (1UL << 3U));
    K_F401RE_GPIOA_OSPEEDR |= ((2UL << (2U * 2U)) | (2UL << (3U * 2U)));
    K_F401RE_GPIOA_PUPDR &= ~((3UL << (2U * 2U)) | (3UL << (3U * 2U)));
    K_F401RE_GPIOA_PUPDR |= (1UL << (3U * 2U));
    K_F401RE_GPIOA_AFRL &= ~((0xFUL << (2U * 4U)) |
                             (0xFUL << (3U * 4U)));
    K_F401RE_GPIOA_AFRL |= ((7UL << (2U * 4U)) | (7UL << (3U * 4U)));
}

/*
 * Decode the APB1 prescaler field from RCC_CFGR. USART2 is clocked from APB1,
 * so the baud-rate divider must use this derived peripheral clock, not the
 * raw core clock.
 */
static unsigned long kBoardConsoleApb1Divisor_(void)
{
    switch ((K_F401RE_RCC_CFGR & K_F401RE_RCC_CFGR_PPRE1_MASK) >>
            K_F401RE_RCC_CFGR_PPRE1_SHIFT)
    {
        case 4UL:
            return (2UL);
        case 5UL:
            return (4UL);
        case 6UL:
            return (8UL);
        case 7UL:
            return (16UL);
        default:
            return (1UL);
    }
}

static unsigned long kBoardConsoleUsart2Brr_(void)
{
    unsigned long coreClock = RK_gSysCoreClock;

    if (coreClock == 0UL)
    {
        coreClock = RK_CONF_EFFECTIVE_SYSCORECLK;
    }
    if (coreClock == 0UL)
    {
        coreClock = 16000000UL;
    }

    unsigned long const apb1Clock = coreClock / kBoardConsoleApb1Divisor_();
    return ((apb1Clock + (RK_BOARD_CONSOLE_BAUD / 2UL)) /
            RK_BOARD_CONSOLE_BAUD);
}

#endif

void kBoardConsoleInit(void)
{
    static unsigned char initDone;

    if (initDone != 0U)
    {
        return;
    }

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
    kBoardConsolePinsInit_();
    /* Configure USART2 as 8N1, 115200 baud, transmitter and receiver enabled. */
    K_F401RE_USART2_CR1 = 0UL;
    K_F401RE_USART2_BRR = kBoardConsoleUsart2Brr_();
    K_F401RE_USART2_CR1 =
        K_F401RE_USART2_CR1_UE | K_F401RE_USART2_CR1_TE |
        K_F401RE_USART2_CR1_RE;
#endif

    initDone = 1U;
}

void kBoardConsoleRxIsrEnable(RK_CONSOLE_RX_ISR_CBK const cbk)
{
    kBoardConsoleInit();

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
    kBoardConsoleRxCbk_ = cbk;
    kBoardConsoleNvicPrioritySet_(K_F401RE_USART2_IRQN,
                                  RK_BOARD_CONSOLE_IRQ_LOWEST_PRIO);
    K_F401RE_USART2_CR1 |= K_F401RE_USART2_CR1_RXNEIE;
    kBoardConsoleNvicEnable_(K_F401RE_USART2_IRQN);
#else
    (void)cbk;
#endif
}

void kPutc(char const c)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        (VOID)kConsoleWrite(&c, 1UL);
        return;
    }

    kBoardConsoleInit();

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
    /* Poll TXE; the console is intentionally simple and synchronous. */
    while ((K_F401RE_USART2_SR & K_F401RE_USART2_SR_TXE) == 0UL)
    {
    }
    K_F401RE_USART2_DR = (unsigned long)((unsigned char)c);
#else
    (void)c;
#endif
}

RK_ERR kConsoleWrite(CHAR const *bufPtr, ULONG bytes)
{
    if (bytes == 0UL)
    {
        return (RK_ERR_SUCCESS);
    }
    if (bufPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    if (bytes > RK_CONSOLE_WRITE_MAX_BYTES)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    if (kSyscallRequired() == RK_TRUE)
    {
        return ((RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_CONSOLE_WRITE, (ULONG)(UINTPTR)bufPtr, bytes,
            0UL, 0UL));
    }

    for (ULONG i = 0UL; i < bytes; i++)
    {
        kPutc(bufPtr[i]);
    }

    return (RK_ERR_SUCCESS);
}

void kPuts(char const *str)
{
    if (str == (char const *)0)
    {
        return;
    }

    while (*str != '\0')
    {
        kPutc(*str);
        str++;
    }
}

int kConsoleGetc(char *chPtr)
{
    if (chPtr == (char *)0)
    {
        return (0);
    }

    kBoardConsoleInit();

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
    if ((K_F401RE_USART2_SR & K_F401RE_USART2_SR_RXNE) == 0UL)
    {
        return (0);
    }

    *chPtr = (char)(K_F401RE_USART2_DR & 0xFFUL);
    return (1);
#else
    return (0);
#endif
}

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
void USART2_IRQHandler(void)
{
    while ((K_F401RE_USART2_SR & K_F401RE_USART2_SR_RXNE) != 0UL)
    {
        BYTE const ch = (BYTE)(K_F401RE_USART2_DR & 0xFFUL);

        if (kBoardConsoleRxCbk_ != NULL)
        {
            kBoardConsoleRxCbk_(ch);
        }
    }
}
#endif

int _write(int file, char const *ptr, int len)
{
    (void)file;

    /*
     * newlib calls _write() for printf-family output. Route all file
     * descriptors to the board console because RK01 has no filesystem.
     */
    for (int i = 0; i < len; i++)
    {
        kPutc(ptr[i]);
    }

    return (len);
}
