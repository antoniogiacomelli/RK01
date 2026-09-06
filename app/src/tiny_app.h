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

/* Static construction parameters for this tiny example. */
#define ECHO_DOMAIN_BYTES (2048U)
#define RECORD_DOMAIN_BYTES (2048U)
#define TASK_STACK_WORDS (256U)
#define APP_LOG_PRIO (10U)
#define ECHO_TASK_PRIO (1)
#define RECORD_TASK_PRIO (2)
#if defined(RK_MCU_F401RE)
#define FS_TASK_PRIO (3)
#endif

/* One extra byte stores the CR or LF terminator inside the shared buffer. */
#define APP_LINE_BYTES ((ULONG)RK_CONF_CONSOLE_LINE_MAX_BYTES)
#define APP_LINE_BUF_BYTES (APP_LINE_BYTES + 1UL)
#define APP_LINE_RING_BYTES (APP_LINE_BUF_BYTES * 4UL)
#define APP_LINE_RING_CAP_BYTES (APP_LINE_RING_BYTES - 1UL)
#define APP_LINE_READY_MAX ((UINT)APP_LINE_RING_CAP_BYTES)

#define RECORD_SLOT_COUNT (4UL)
#define RECORD_PERSIST_TEXT_BYTES (9UL)

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

/*
 * A RECORD is the binary payload exchanged between domains.
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
    RECORD record;
    BYTE valid;
} RecordSlot;

/*
 * RecordTask owns this ring. Other domains use copied call/reply messages, so
 * no caller can keep a raw pointer into the record slots.
 */
typedef struct
{
    RecordSlot slots[RECORD_SLOT_COUNT];
    BYTE nextSeq;
} RecordState;

typedef struct
{
    volatile ULONG readPos;      /* Next bytes[] slot EchoTask will read. */
    volatile ULONG writePos;     /* Next slot the console service will fill. */
    volatile ULONG lineBytes;    /* Current unterminated line length. */
    volatile RK_BOOL dropLf;     /* CRLF suppression state. */
    volatile BYTE bytes[APP_LINE_RING_BYTES];
} AppLineRing;

_Static_assert(sizeof(RECORD) == 4UL,
               "RECORD must stay BYTE seq, BYTE nome, USHORT valor");
_Static_assert((sizeof(RecordRequest) % RK_WORD_SIZE) == 0UL,
               "RecordRequest must be word-sized for call/reply");
_Static_assert((sizeof(RecordReply) % RK_WORD_SIZE) == 0UL,
               "RecordReply must be word-sized for call/reply");

extern RK_TASK_HANDLE recordTaskHandle;
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
VOID RecordTask(VOID *args);

#endif
