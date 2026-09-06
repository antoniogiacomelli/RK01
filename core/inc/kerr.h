/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_ERR_H
#define RK_ERR_H

#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kobjs.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C"
{
#endif

extern volatile RK_FAULT RK_gFaultID;
struct traceItem
{
    ULONG magic;
    ULONG buildCookie;
    RK_FAULT code;
    RK_TICK tick;
    UINT sp;
    CHAR *task;
    BYTE taskID;
    UINT lr;
} K_ALIGN(4);

VOID kPanic(const char *fileName, const int line, const char *fmt, ...)
__attribute__((format(printf, 3, 4)));

#define K_PANIC(...)\
        do\
        {\
            kPanic(__FILE__, __LINE__, __VA_ARGS__);\
        } while (0)

__attribute__((section(".noinit")))
extern volatile struct traceItem RK_gTraceInfo;
VOID kFaultTraceInit(VOID);
VOID kFaultTraceClear(VOID);
VOID kErrHandler(RK_FAULT);

#ifdef __cplusplus
}
#endif
#endif /* RK_ERR_H*/
