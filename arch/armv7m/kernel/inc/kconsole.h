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
#define RK_CONSOLE_WRITE_MAX_BYTES RK_CONF_CONSOLE_WRITE_MAX_BYTES
#endif

typedef VOID (*RK_CONSOLE_RX_CBK)(BYTE ch);
typedef RK_CONSOLE_RX_CBK RK_CONSOLE_RX_ISR_CBK;

/*
 * Initialise the board console used by the kernel fault/debug path.
 * Current board bindings: STM32F401RE USART2 on PA2/PA3, and MPS2 AN386
 * CMSDK UART0 routed by QEMU to the first serial chardev.
 * Normal task I/O should use the privileged UART service, not this raw path.
 */
void kBoardConsoleInit(void);

/* Start the privileged kernel UART driver service. */
RK_ERR kConsoleServiceInit(VOID);

/*
 * Claim foreground RX ownership for a non-blocking byte consumer. The callback
 * runs from the privileged console driver task, not from the UART ISR.
 */
RK_ERR kConsoleRxClaim(RK_CONSOLE_RX_CBK cbk);

/*
 * Release foreground RX ownership. The caller must pass the currently claimed
 * callback; releasing another owner's callback returns RK_ERR_NOT_OWNER.
 */
RK_ERR kConsoleRxRelease(RK_CONSOLE_RX_CBK cbk);

/*
 * Compatibility wrapper for older code. It now claims foreground RX ownership
 * through the kernel UART service instead of installing cbk as the ISR owner.
 */
void kBoardConsoleRxIsrEnable(RK_CONSOLE_RX_ISR_CBK cbk);

/* Blocking single-byte transmit. Enters SVC from unprivileged tasks. */
void kPutc(char const c);

/* Bounded call/reply transmit through the privileged UART service. */
RK_ERR kConsoleWrite(CHAR const *bufPtr, ULONG bytes);

/* Blocking NUL-terminated string transmit. A NULL pointer is ignored. */
void kPuts(char const *str);

/*
 * Raw nonblocking board receive. Normal consumers should claim foreground RX
 * ownership through kConsoleRxClaim(); this fallback is for early/trusted paths.
 */
int kConsoleGetc(char *chPtr);

#ifdef __cplusplus
}
#endif

#endif /* RK_CONSOLE_H */
