/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RECORD_DOMAIN_H
#define RECORD_DOMAIN_H

#include <kapi_domain.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * A RECORD is the binary payload exchanged with the Record domain.
 *
 *   RECORD:
 *   +------+-------+-------------+
 *   | seq  | nome  | valor       |
 *   | BYTE | BYTE  | USHORT      |
 *   +------+-------+-------------+
 *      0      1       2..3
 */
typedef struct
{
    BYTE seq;
    BYTE nome;
    USHORT valor;
} RECORD;

typedef enum
{
    RECORD_CMD_WRITE = 1,
    RECORD_CMD_READ = 2
} RecordCommand;

typedef enum
{
    RECORD_STATUS_OK = 0,
    RECORD_STATUS_NOT_FOUND = 1,
    RECORD_STATUS_INVALID = 2,
    RECORD_STATUS_STORAGE = 3
} RecordStatus;

typedef struct
{
    ULONG command;
    RECORD record;
} RecordRequest;

typedef struct
{
    ULONG status;
    RECORD record;
} RecordReply;

typedef struct
{
    RK_TASK_HANDLE serviceHandle;
} RECORD_DOMAIN_EXPORTS;

_Static_assert(sizeof(RECORD) == 4UL,
               "RECORD must stay BYTE seq, BYTE nome, USHORT valor");
_Static_assert((sizeof(RecordRequest) % RK_WORD_SIZE) == 0UL,
               "RecordRequest must be word-sized for call/reply");
_Static_assert((sizeof(RecordReply) % RK_WORD_SIZE) == 0UL,
               "RecordReply must be word-sized for call/reply");

RK_ERR RecordDomainBoot(RECORD_DOMAIN_EXPORTS *exportsPtr);

#ifdef __cplusplus
}
#endif

#endif
