/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef KLOGGER_H
#define KLOGGER_H

#include <kcommondefs.h>
#include <kconfig.h>

#define CONF_LOGGER 1 /* Turn logger on/off. */

#if (CONF_LOGGER == 1)
#define LOGLEN 64         /* Max length of a single log message. */
#define LOGPOOLSIZ 16     /* Number of log message buffers. */
#define LOG_STACKSIZE 512 /* Size of the stack. */

/* Used by kLog and kLogError. */
#define LOG_LEVEL_MSG           0
#define LOG_LEVEL_FAULT         1

/* Print "E" on the console to warn about pool exhaustion. */
#define CONF_LOG_ERROR (ON)


#if (RK_CONF_MESG_QUEUE == OFF)
#error "Need RK_CONF_MESG_QUEUE enabled for logger facility"
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

VOID kLogInit(RK_PRIO priority);
VOID kLogWrite(UINT level, const char *fmt, ...)
__attribute__((format(printf, 2, 3)));
/* Pauses or restores normal LOG_LEVEL_MSG console draining. Fault output still
 * prints immediately. */
VOID kLogNormalOutputSet(RK_BOOL enabled);
RK_BOOL kLogNormalOutputGet(VOID);

#define kLog(...)      kLogWrite(LOG_LEVEL_MSG, __VA_ARGS__)
#define kLogError(...) kLogWrite(LOG_LEVEL_FAULT, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* KLOGGER_H */
