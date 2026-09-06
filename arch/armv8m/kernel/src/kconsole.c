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
 *   MPS2 AN505 board-console backend. QEMU wires UART0 to the first serial
 *   chardev, so a small CMSDK APB UART driver is enough for kernel
 *   diagnostics, the sanity application log and optional RX interrupt delivery.
 */

#define RK_SOURCE_CODE

#include <kconsole.h>
#include <kconfig.h>
#include <kcoredefs.h>
#include <ksyscall.h>

#if defined(RK_MCU_MPS2_AN505)
#define RK_BOARD_CONSOLE_HAS_CMSDK_UART (1U)

#define MPS2_UART0_BASE (0x40200000UL)
#define CMSDK_UART_DATA (*(volatile unsigned long *)(MPS2_UART0_BASE + 0x00UL))
#define CMSDK_UART_STATE (*(volatile unsigned long *)(MPS2_UART0_BASE + 0x04UL))
#define CMSDK_UART_CTRL (*(volatile unsigned long *)(MPS2_UART0_BASE + 0x08UL))
#define CMSDK_UART_INTSTATUS (*(volatile unsigned long *)(MPS2_UART0_BASE + 0x0CUL))
#define CMSDK_UART_BAUDDIV (*(volatile unsigned long *)(MPS2_UART0_BASE + 0x10UL))

#define CMSDK_UART_STATE_TXBF (1UL << 0U)
#define CMSDK_UART_STATE_RXBF (1UL << 1U)
#define CMSDK_UART_CTRL_TXEN (1UL << 0U)
#define CMSDK_UART_CTRL_RXEN (1UL << 1U)
#define CMSDK_UART_CTRL_RXIRQEN (1UL << 3U)
#define CMSDK_UART_INTSTATUS_RXIRQ (1UL << 1U)
#define CMSDK_UART_BAUD (115200UL)
#define CMSDK_UART0_IRQN (32UL)
#endif

#if defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
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

void kBoardConsoleInit(void)
{
    static unsigned char initDone;

    if (initDone != 0U)
    {
        return;
    }

#if defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
    unsigned long coreClock = RK_gSysCoreClock;

    if (coreClock == 0UL)
    {
        coreClock = RK_CONF_EFFECTIVE_SYSCORECLK;
    }
    if (coreClock == 0UL)
    {
        coreClock = 20000000UL;
    }

    CMSDK_UART_CTRL = 0UL;
    CMSDK_UART_BAUDDIV =
        ((coreClock + (CMSDK_UART_BAUD / 2UL)) / CMSDK_UART_BAUD);
    CMSDK_UART_CTRL = CMSDK_UART_CTRL_TXEN | CMSDK_UART_CTRL_RXEN;
#endif

    initDone = 1U;
}

void kBoardConsoleRxIsrEnable(RK_CONSOLE_RX_ISR_CBK const cbk)
{
    kBoardConsoleInit();

#if defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
    kBoardConsoleRxCbk_ = cbk;
    kBoardConsoleNvicPrioritySet_(CMSDK_UART0_IRQN,
                                  RK_BOARD_CONSOLE_IRQ_LOWEST_PRIO);
    CMSDK_UART_INTSTATUS = CMSDK_UART_INTSTATUS_RXIRQ;
    CMSDK_UART_CTRL |= CMSDK_UART_CTRL_RXIRQEN;
    kBoardConsoleNvicEnable_(CMSDK_UART0_IRQN);
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

#if defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
    while ((CMSDK_UART_STATE & CMSDK_UART_STATE_TXBF) != 0UL)
    {
    }
    CMSDK_UART_DATA = (unsigned long)((unsigned char)c);
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

#if defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
    if ((CMSDK_UART_STATE & CMSDK_UART_STATE_RXBF) == 0UL)
    {
        return (0);
    }

    *chPtr = (char)(CMSDK_UART_DATA & 0xFFUL);
    return (1);
#else
    return (0);
#endif
}

#if defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
void UART0_Handler(void)
{
    do
    {
        CMSDK_UART_INTSTATUS = CMSDK_UART_INTSTATUS_RXIRQ;
        while ((CMSDK_UART_STATE & CMSDK_UART_STATE_RXBF) != 0UL)
        {
            BYTE const ch = (BYTE)(CMSDK_UART_DATA & 0xFFUL);

            if (kBoardConsoleRxCbk_ != NULL)
            {
                kBoardConsoleRxCbk_(ch);
            }
        }
    }
    while (((CMSDK_UART_STATE & CMSDK_UART_STATE_RXBF) != 0UL) ||
           ((CMSDK_UART_INTSTATUS & CMSDK_UART_INTSTATUS_RXIRQ) != 0UL));
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
