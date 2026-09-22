/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_TASKSUSPEND_H
#define RK_TASKSUSPEND_H

#ifdef __cplusplus
extern "C" {
#endif

#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kobjs.h>

RK_ERR kTaskSelfSuspend(VOID);
RK_ERR kTaskResume(RK_TASK_HANDLE const taskHandle);

#ifdef __cplusplus
}
#endif

#endif /* RK_TASKSUSPEND_H */
