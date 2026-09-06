/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_TASKFLAGS_H
#define RK_TASKFLAGS_H

#ifdef __cplusplus
extern "C" {
#endif
#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kobjs.h>
RK_ERR kEventGet(RK_TASK_EVENT const, RK_OPTION const, RK_TASK_EVENT* const, RK_TICK const);
RK_ERR kEventSet(RK_TASK_HANDLE const, RK_TASK_EVENT const);
RK_ERR kEventClear(RK_TASK_HANDLE, RK_TASK_EVENT const);
RK_ERR kEventQuery(RK_TASK_HANDLE const, RK_TASK_EVENT* const);

#ifdef __cplusplus
}
#endif

#endif
