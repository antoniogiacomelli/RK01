/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_SIGNAL_H
#define RK_SIGNAL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <kcommondefs.h>
#include <kmpu.h>
#include <kobjs.h>

RK_ERR kSignalHandlerSet(RK_SIGNAL const signal,
                         RK_SIGNAL_HANDLER const handler,
                         VOID *const altStackBasePtr,
                         ULONG const altStackBytes);
RK_ERR kSignalSend(RK_TASK_HANDLE const taskHandle, RK_SIGNAL const signal);
RK_ERR kSignalMaskSet(RK_SIGNAL const enabledMask);
RK_ERR kSignalReturn(VOID);

VOID kSignalTaskCleanup(RK_TCB *const taskPtr);
VOID *kSignalMaybeDeliverOnReturn(RK_EXCEPTION_FRAME *const framePtr);
VOID kSignalMaybeDeliverOnSvcExit(RK_EXCEPTION_FRAME *const framePtr);

#ifdef __cplusplus
}
#endif

#endif /* RK_SIGNAL_H */
