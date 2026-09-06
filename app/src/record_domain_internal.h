/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RECORD_DOMAIN_INTERNAL_H
#define RECORD_DOMAIN_INTERNAL_H

#include "record_domain.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define RECORD_DOMAIN_BYTES (2048U)
#define RECORD_TASK_STACK_WORDS (256U)
#define RECORD_TASK_PRIO (2)
#define RECORD_SLOT_COUNT (4UL)
#define RECORD_PERSIST_TEXT_BYTES (9UL)

typedef struct
{
    RECORD record;
    BYTE valid;
} RecordSlot;

/*
 * Record server tasks may include this header. Domain clients include only
 * record_domain.h and use copied call/reply messages.
 */
typedef struct
{
    RecordSlot slots[RECORD_SLOT_COUNT];
    BYTE nextSeq;
} RecordState;

RK_DECLARE_DOMAIN_RAM(RECORD_DOMAIN_RAM,
    RK_DOMAIN_RAM_STACK(serverStack, RECORD_TASK_STACK_WORDS)
    RK_DOMAIN_RAM_MEMBER(RecordState, recordState)
    RK_DOMAIN_RAM_TASK_HANDLE(serverHandle)
)

_Static_assert((RECORD_TASK_STACK_WORDS % 2U) == 0U,
               "Record task stack must be word-even");
_Static_assert(sizeof(RECORD_DOMAIN_RAM) <= RECORD_DOMAIN_BYTES,
               "Record domain RAM layout must fit its MPU window");

VOID RecordTask(VOID *args);

#ifdef __cplusplus
}
#endif

#endif
