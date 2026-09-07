/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef TINY_APP_H
#define TINY_APP_H

#include <kapi.h>
#include <kconsole.h>
#include <kstring.h>

#include "record_domain.h"

/* Static construction parameters for this tiny example. */
#define ECHO_DOMAIN_BYTES (1024U)
#define TASK_STACK_WORDS (256U)
#define APP_LOG_PRIO (10U)
#define ECHO_TASK_PRIO (1)
#if defined(RK_MCU_F401RE)
#define FS_TASK_PRIO (3)
#endif

/* One extra byte stores the CR or LF terminator inside the shared buffer. */
#define APP_LINE_BYTES ((ULONG)RK_CONF_CONSOLE_LINE_MAX_BYTES)
#define APP_LINE_BUF_BYTES (APP_LINE_BYTES + 1UL)
#define APP_LINE_RING_BYTES (APP_LINE_BUF_BYTES * 4UL)
#define APP_LINE_RING_CAP_BYTES (APP_LINE_RING_BYTES - 1UL)
#define APP_LINE_READY_MAX ((UINT)APP_LINE_RING_CAP_BYTES)

#if defined(RK_MCU_F401RE)
#define APP_CONSOLE_NAME "USART2"
#else
#define APP_CONSOLE_NAME "console"
#endif
#define APP_SYSMON_TOKEN "RKMONITOR"

_Static_assert(APP_LINE_BUF_BYTES <= RK_CONSOLE_WRITE_MAX_BYTES,
               "Echo line must fit one bounded console write");
_Static_assert(APP_LINE_RING_CAP_BYTES > APP_LINE_BUF_BYTES,
               "Line ring must hold at least one complete line");
_Static_assert(APP_LINE_RING_CAP_BYTES <= (ULONG)RK_UINT_MAX,
               "Line-ready semaphore max must fit UINT");

typedef struct
{
    volatile ULONG readPos;      /* Next bytes[] slot EchoTask will read. */
    volatile ULONG writePos;     /* Next slot the console service will fill. */
    volatile ULONG lineBytes;    /* Current unterminated line length. */
    volatile RK_BOOL dropLf;     /* CRLF suppression state. */
    volatile BYTE bytes[APP_LINE_RING_BYTES];
} AppLineRing;

extern RK_SEMAPHORE_HANDLE lineReadySemaHandle;
extern AppLineRing sharedLineRing;

#if defined(RK_MCU_F401RE)
extern RK_TASK_HANDLE fsTaskHandle;
#endif

VOID AppCheck_(RK_ERR const err);
VOID *AppCheckPtr_(VOID *const ptr);
VOID AppConsoleWriteText_(CHAR const *const textPtr);

VOID AppLineByteFromConsole_(BYTE const ch);
VOID EchoTask(VOID *args);

#endif
