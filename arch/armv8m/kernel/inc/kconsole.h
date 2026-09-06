/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_CONSOLE_H
#define RK_CONSOLE_H

#include <kcommondefs.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RK_CONSOLE_WRITE_MAX_BYTES
#define RK_CONSOLE_WRITE_MAX_BYTES (128UL)
#endif

typedef VOID (*RK_CONSOLE_RX_ISR_CBK)(BYTE ch);

/*
 * Initialise the board console used by the kernel fault/debug path.
 * Current board binding: MPS2 AN505 CMSDK UART0, routed by QEMU to the first
 * serial chardev.
 */
void kBoardConsoleInit(void);

/* Enable board-console RX IRQ delivery to a byte callback. Privileged only. */
void kBoardConsoleRxIsrEnable(RK_CONSOLE_RX_ISR_CBK cbk);

/* Blocking single-byte transmit. Safe for simple kernel diagnostics. */
void kPutc(char const c);

/* Bounded blocking transmit. Enters SVC when called from unprivileged tasks. */
RK_ERR kConsoleWrite(CHAR const *bufPtr, ULONG bytes);

/* Blocking NUL-terminated string transmit. A NULL pointer is ignored. */
void kPuts(char const *str);

/* Nonblocking single-byte receive. Returns 1 when a byte is copied, else 0. */
int kConsoleGetc(char *chPtr);

#ifdef __cplusplus
}
#endif

#endif /* RK_CONSOLE_H */
