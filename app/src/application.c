/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Default application: interrupt-driven board-console RX to application tasks.
 *
 * The target-specific console IRQ writes received bytes into a shared circular
 * buffer. CR, LF and CRLF terminal endings are accepted. When a complete line
 * is registered, the ISR posts a global counting semaphore. EchoTask consumes
 * one semaphore count per complete line, parses console commands and calls the
 * Record module through synchronous copied messages.
 */

#include <kapi.h>
#include <kapi_trusted.h>
#include <kconsole.h>
#include <klogger.h>
#include <kstring.h>

#if defined(RK_MCU_F401RE)
#include <rkfs.h>
#endif

/* Static construction parameters for this tiny example. */
#define ECHO_MODULE_BYTES (2048U)
#define RECORD_MODULE_BYTES (2048U)
#define TASK_STACK_WORDS (256U)
#define APP_LOG_PRIO (10U)
#define ECHO_TASK_PRIO (1)
#define RECORD_TASK_PRIO (2)
#if defined(RK_MCU_F401RE)
#define FS_TASK_PRIO (3)
#endif

/* One extra byte stores the CR or LF terminator inside the shared buffer. */
#define APP_LINE_BYTES (64UL)
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

_Static_assert(APP_LINE_BUF_BYTES <= RK_CONSOLE_WRITE_MAX_BYTES,
               "Echo line must fit one bounded console write");
_Static_assert(APP_LINE_RING_CAP_BYTES > APP_LINE_BUF_BYTES,
               "Line ring must hold at least one complete line");
_Static_assert(APP_LINE_RING_CAP_BYTES <= (ULONG)RK_UINT_MAX,
               "Line-ready semaphore max must fit UINT");

/*
 * A RECORD is the binary payload exchanged between modules.
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
 * RecordTask owns this ring. The console module never writes these slots
 * directly; it sends a RecordRequest and receives a RecordReply.
 *
 *   slots[]:
 *   +---------+---------+---------+---------+
 *   | slot 0  | slot 1  | slot 2  | slot 3  |
 *   +---------+---------+---------+---------+
 *        ^ oldest                    ^ newest
 *
 * Once the four slots are valid, the next WRITE replaces the slot containing
 * the oldest seq. READ nome searches the newest matching nome.
 */
typedef struct
{
    RecordSlot slots[RECORD_SLOT_COUNT];
    BYTE nextSeq;
} RecordState;

_Static_assert(sizeof(RECORD) == 4UL,
               "RECORD must stay BYTE seq, BYTE nome, USHORT valor");
_Static_assert((sizeof(RecordRequest) % RK_WORD_SIZE) == 0UL,
               "RecordRequest must be word-sized for synchronous messages");
_Static_assert((sizeof(RecordReply) % RK_WORD_SIZE) == 0UL,
               "RecordReply must be word-sized for synchronous messages");

/*
 * ISR-to-task line ring.
 *
 * The UART IRQ writes each byte into bytes[]. When Enter arrives, the IRQ also
 * writes the CR or LF byte. That delimiter is the in-band "line complete"
 * marker. The counting semaphore below counts how many such delimiters are
 * ready for EchoTask.
 *
 *   bytes[]:
 *   +---+---+----+---+---+         +---+
 *   | a | b | \r |   |   |   ...   |   |
 *   +---+---+----+---+---+         +---+
 *    ^            ^
 *    |            |
 *    readPos      writePos
 *
 * The ring leaves one byte unused, so the pointer states are unambiguous:
 *
 *   empty: readPos == writePos
 *   full:  next(writePos) == readPos
 *
 * Available room is derived from readPos/writePos. We do not store a room
 * field because both the IRQ and the task would have to update it atomically.
 *
 *   UART IRQ owns: writePos and bytes[writePos]
 *   EchoTask owns: readPos
 */
typedef struct
{
    volatile ULONG readPos;      /* Next bytes[] slot EchoTask will read. */
    volatile ULONG writePos;     /* Next bytes[] slot the UART IRQ will fill. */
    volatile BYTE bytes[APP_LINE_RING_BYTES];
} AppLineRing;

/*
 * EchoTask runs in its own explicit module.
 *
 * The module boundary is deliberate: EchoTask can access echoRam and shared
 * RAM, but it must not depend on ordinary App-module globals. This is why the
 * line ring is placed in .rk_shared_ram.
 */
RK_DECLARE_MODULE(echoModule, echoRam, ECHO_MODULE_BYTES)
RK_DECLARE_MODULE_TASK(echoTaskHandle, EchoTask)
RK_DECLARE_MODULE(recordModule, recordRam, RECORD_MODULE_BYTES)
RK_DECLARE_MODULE_TASK(recordTaskHandle, RecordTask)
static RK_DECLARE_GLOBAL_SEMAPHORE(lineReadySemaHandle)

#if defined(RK_MCU_F401RE)
static RKFS_RAM fsRam RK_MODULE_RAM_ATTR(RKFS_MODULE_BYTES);
static RK_MODULE fsModule RK_MODULE_DESC_ATTR;
RK_DECLARE_MODULE_TASK(fsTaskHandle, rkFsServerTask)
#endif

static RecordState *recordState;

/*
 * Shared ISR-to-task circular line buffer.
 *
 * The ISR writes ordinary bytes and the CR/LF delimiter into this ring. The
 * delimiter marks a complete line, and lineReadySemaHandle counts completed
 * delimiters, so no message queue object, per-message payload or stored length
 * field is needed.
 *
 * The ring lives in shared RAM because it is touched by handler mode and by an
 * unprivileged module task. The small CR/LF parser variables below are
 * ISR-owned only, so they can remain ordinary application globals.
 */
static AppLineRing sharedLineRing K_ALIGN(4) RK_SHARED_RAM_ATTR;
static RK_BOOL isrDropLf;
static ULONG isrLineBytes;

/*
 * Fail fast during example construction/runtime.
 *
 * Real applications can recover from selected positive outcomes such as a
 * timeout or a full object. This example treats every non-success return as a
 * programming error so object-scope mistakes are immediately visible.
 */
RK_FORCE_INLINE
static inline VOID AppCheck_(RK_ERR const err)
{
    K_ASSERT(err == RK_ERR_SUCCESS);
    if (err != RK_ERR_SUCCESS)
    {
        while (1)
        {
            kErrHandler((RK_FAULT)err);
        }
    }
}

RK_FORCE_INLINE
static inline VOID *AppCheckPtr_(VOID *const ptr)
{
    K_ASSERT(ptr != NULL);
    if (ptr == NULL)
    {
        while (1)
        {
            kErrHandler(RK_FAULT_INVALID_PARAM);
        }
    }

    return (ptr);
}

static ULONG AppTextLen_(CHAR const *const textPtr, ULONG const maxBytes)
{
    ULONG bytes = 0UL;

    while ((bytes < maxBytes) && (textPtr[bytes] != '\0'))
    {
        bytes++;
    }

    return (bytes);
}

static VOID AppConsoleWriteText_(CHAR const *const textPtr)
{
    AppCheck_(kConsoleWrite(textPtr,
                            AppTextLen_(textPtr,
                                        RK_CONSOLE_WRITE_MAX_BYTES)));
}

static BYTE AppUpper_(BYTE const ch)
{
    if ((ch >= (BYTE)'a') && (ch <= (BYTE)'z'))
    {
        return ((BYTE)(ch - ((BYTE)'a' - (BYTE)'A')));
    }

    return (ch);
}

static RK_BOOL AppIsSpace_(BYTE const ch)
{
    return (((ch == (BYTE)' ') || (ch == (BYTE)'\t')) ? RK_TRUE : RK_FALSE);
}

static RK_BOOL AppIsSeparator_(BYTE const ch)
{
    return (((AppIsSpace_(ch) == RK_TRUE) || (ch == (BYTE)',') ||
             (ch == (BYTE)'=')) ?
                RK_TRUE :
                RK_FALSE);
}

static VOID AppSkipSpaces_(BYTE const *const linePtr,
                           ULONG const lineBytes,
                           ULONG *const posPtr)
{
    while ((*posPtr < lineBytes) &&
           (AppIsSpace_(linePtr[*posPtr]) == RK_TRUE))
    {
        (*posPtr)++;
    }
}

static RK_BOOL AppOnlySpaces_(BYTE const *const linePtr,
                              ULONG const lineBytes,
                              ULONG const pos)
{
    ULONG i = pos;

    while (i < lineBytes)
    {
        if (AppIsSpace_(linePtr[i]) != RK_TRUE)
        {
            return (RK_FALSE);
        }
        i++;
    }

    return (RK_TRUE);
}

static RK_BOOL AppTokenEquals_(BYTE const *const linePtr,
                               ULONG const tokenStart,
                               ULONG const tokenBytes,
                               CHAR const *const textPtr)
{
    ULONG i = 0UL;

    while (i < tokenBytes)
    {
        if (textPtr[i] == '\0')
        {
            return (RK_FALSE);
        }
        if (AppUpper_(linePtr[tokenStart + i]) !=
            AppUpper_((BYTE)textPtr[i]))
        {
            return (RK_FALSE);
        }
        i++;
    }

    return ((textPtr[i] == '\0') ? RK_TRUE : RK_FALSE);
}

static RK_BOOL AppParseName_(BYTE const *const linePtr,
                             ULONG const lineBytes,
                             ULONG *const posPtr,
                             BYTE *const namePtr)
{
    BYTE name;

    AppSkipSpaces_(linePtr, lineBytes, posPtr);
    if (*posPtr >= lineBytes)
    {
        return (RK_FALSE);
    }

    name = linePtr[*posPtr];
    if ((name < (BYTE)'!') || (name > (BYTE)'~') ||
        (AppIsSeparator_(name) == RK_TRUE))
    {
        return (RK_FALSE);
    }
    (*posPtr)++;

    if ((*posPtr < lineBytes) &&
        (AppIsSeparator_(linePtr[*posPtr]) != RK_TRUE))
    {
        return (RK_FALSE);
    }

    *namePtr = name;
    return (RK_TRUE);
}

static VOID AppSkipValueSeparators_(BYTE const *const linePtr,
                                    ULONG const lineBytes,
                                    ULONG *const posPtr)
{
    while ((*posPtr < lineBytes) &&
           (AppIsSeparator_(linePtr[*posPtr]) == RK_TRUE))
    {
        (*posPtr)++;
    }
}

static RK_BOOL AppParseUshort_(BYTE const *const linePtr,
                               ULONG const lineBytes,
                               ULONG *const posPtr,
                               USHORT *const valuePtr)
{
    ULONG value = 0UL;
    RK_BOOL gotDigit = RK_FALSE;

    AppSkipValueSeparators_(linePtr, lineBytes, posPtr);
    while (*posPtr < lineBytes)
    {
        BYTE const ch = linePtr[*posPtr];

        if ((ch < (BYTE)'0') || (ch > (BYTE)'9'))
        {
            break;
        }

        gotDigit = RK_TRUE;
        value = (value * 10UL) + (ULONG)(ch - (BYTE)'0');
        if (value > 65535UL)
        {
            return (RK_FALSE);
        }
        (*posPtr)++;
    }

    if (gotDigit != RK_TRUE)
    {
        return (RK_FALSE);
    }

    *valuePtr = (USHORT)value;
    return (RK_TRUE);
}

static RK_BOOL AppParseRecordCommand_(BYTE const *const linePtr,
                                      ULONG const lineBytes,
                                      RecordRequest *const reqPtr)
{
    ULONG pos = 0UL;
    ULONG tokenStart;
    ULONG tokenBytes;
    BYTE name = 0U;
    USHORT value = 0U;

    RK_MEMSET(reqPtr, 0, sizeof(*reqPtr));
    AppSkipSpaces_(linePtr, lineBytes, &pos);
    if (pos >= lineBytes)
    {
        return (RK_FALSE);
    }

    tokenStart = pos;
    while ((pos < lineBytes) &&
           (AppIsSeparator_(linePtr[pos]) != RK_TRUE))
    {
        pos++;
    }
    tokenBytes = pos - tokenStart;

    if (AppTokenEquals_(linePtr, tokenStart, tokenBytes, "READ") == RK_TRUE)
    {
        if (AppParseName_(linePtr, lineBytes, &pos, &name) != RK_TRUE)
        {
            return (RK_FALSE);
        }
        if (AppOnlySpaces_(linePtr, lineBytes, pos) != RK_TRUE)
        {
            return (RK_FALSE);
        }

        reqPtr->command = (ULONG)RECORD_CMD_READ;
        reqPtr->record.nome = name;
        return (RK_TRUE);
    }

    if ((AppTokenEquals_(linePtr, tokenStart, tokenBytes, "SET") ==
         RK_TRUE) ||
        (AppTokenEquals_(linePtr, tokenStart, tokenBytes, "WRITE") ==
         RK_TRUE) ||
        (AppTokenEquals_(linePtr, tokenStart, tokenBytes, "RECORD") ==
         RK_TRUE))
    {
        if (AppParseName_(linePtr, lineBytes, &pos, &name) != RK_TRUE)
        {
            return (RK_FALSE);
        }
        if (AppParseUshort_(linePtr, lineBytes, &pos, &value) != RK_TRUE)
        {
            return (RK_FALSE);
        }
        if (AppOnlySpaces_(linePtr, lineBytes, pos) != RK_TRUE)
        {
            return (RK_FALSE);
        }

        reqPtr->command = (ULONG)RECORD_CMD_WRITE;
        reqPtr->record.nome = name;
        reqPtr->record.valor = value;
        return (RK_TRUE);
    }

    if (tokenBytes == 1UL)
    {
        pos = tokenStart;
        if (AppParseName_(linePtr, lineBytes, &pos, &name) == RK_TRUE)
        {
            if (AppParseUshort_(linePtr, lineBytes, &pos, &value) ==
                RK_TRUE)
            {
                if (AppOnlySpaces_(linePtr, lineBytes, pos) == RK_TRUE)
                {
                    reqPtr->command = (ULONG)RECORD_CMD_WRITE;
                    reqPtr->record.nome = name;
                    reqPtr->record.valor = value;
                    return (RK_TRUE);
                }
            }
        }
    }

    return (RK_FALSE);
}

static RK_ERR AppRecordCall_(RecordRequest *const reqPtr,
                             RecordReply *const replyPtr)
{
    RK_SYNCH_ATTR attr;
    ULONG replyBytes = 0UL;
    RK_ERR err;

    attr.reqPtr = reqPtr;
    attr.reqBytes = sizeof(*reqPtr);
    attr.replyPtr = replyPtr;
    attr.replyMaxBytes = sizeof(*replyPtr);
    attr.replyBytesPtr = &replyBytes;

    err = kSynchMesgCall(recordTaskHandle, &attr, RK_WAIT_FOREVER);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
    if (replyBytes != sizeof(*replyPtr))
    {
        return (RK_ERR_INVALID_MSG_SIZE);
    }

    return (RK_ERR_SUCCESS);
}

static VOID AppUlongToText_(ULONG value,
                            CHAR *const textPtr,
                            ULONG const textBytes)
{
    CHAR reverse[10];
    ULONG digits = 0UL;
    ULONG out = 0UL;

    if (textBytes == 0UL)
    {
        return;
    }

    do
    {
        reverse[digits] = (CHAR)('0' + (CHAR)(value % 10UL));
        value /= 10UL;
        digits++;
    } while ((value != 0UL) && (digits < sizeof(reverse)));

    while ((digits > 0UL) && (out < (textBytes - 1UL)))
    {
        digits--;
        textPtr[out] = reverse[digits];
        out++;
    }

    textPtr[out] = '\0';
}

static VOID AppConsoleWriteRecord_(CHAR const *const verbPtr,
                                   RECORD const *const recordPtr)
{
    CHAR valueText[6];
    CHAR seqText[4];
    CHAR nameText[2];

    AppUlongToText_((ULONG)recordPtr->valor, valueText, sizeof(valueText));
    AppUlongToText_((ULONG)recordPtr->seq, seqText, sizeof(seqText));
    nameText[0] = (CHAR)recordPtr->nome;
    nameText[1] = '\0';

    AppConsoleWriteText_(verbPtr);
    AppConsoleWriteText_(" ");
    AppConsoleWriteText_(nameText);
    AppConsoleWriteText_("=");
    AppConsoleWriteText_(valueText);
    AppConsoleWriteText_(" seq=");
    AppConsoleWriteText_(seqText);
    AppConsoleWriteText_("\r\n");
}

static ULONG AppLineRingNext_(ULONG const pos)
{
    ULONG next = pos + 1UL;

    if (next >= APP_LINE_RING_BYTES)
    {
        next = 0UL;
    }

    return (next);
}

/*
 * Compute free bytes from the two positions.
 *
 * This is called by the producer. If EchoTask advances readPos at the same
 * time, the ISR may briefly underestimate free room, which is safe: at worst a
 * byte is dropped even though space just became available.
 */
static ULONG AppLineRingFree_(VOID)
{
    ULONG const readPos = sharedLineRing.readPos;
    ULONG const writePos = sharedLineRing.writePos;

    if (writePos >= readPos)
    {
        return (APP_LINE_RING_BYTES - (writePos - readPos) - 1UL);
    }

    return (readPos - writePos - 1UL);
}

/*
 * Add one byte to the ISR-owned producer side of the ring.
 *
 * Ordinary data bytes reserve one extra slot for the future CR/LF terminator,
 * so a partially typed line cannot consume the last byte needed to publish it.
 */
static RK_BOOL AppLineRingWriteFromIsr_(BYTE const ch, RK_BOOL const reserveTerm)
{
    ULONG const neededFree = (reserveTerm == RK_TRUE) ? 2UL : 1UL;
    ULONG const freeBytes = AppLineRingFree_();

    if (freeBytes < neededFree)
    {
        return (RK_FALSE);
    }

    sharedLineRing.bytes[sharedLineRing.writePos] = ch;
    RK_DMB
    sharedLineRing.writePos = AppLineRingNext_(sharedLineRing.writePos);

    return (RK_TRUE);
}

/*
 * Publish one completed line from interrupt context.
 *
 * The terminator is a real byte in the shared ring. EchoTask later reads bytes
 * until it sees this terminator.
 */
static VOID AppLineSubmitFromIsr_(BYTE const terminator)
{
    if (AppLineRingWriteFromIsr_(terminator, RK_FALSE) == RK_TRUE)
    {
        if (lineReadySemaHandle != RK_NULL_HANDLE)
        {
            (VOID)kSemaphorePost(lineReadySemaHandle);
        }
    }

    isrLineBytes = 0UL;
}

/*
 * Console RX byte callback, called by the board-specific UART IRQ handler.
 *
 * CR, LF and CRLF are accepted as Enter. For CRLF, the CR submits the line and
 * the following LF is consumed by isrDropLf so the terminal does not produce an
 * empty second line.
 */
static VOID AppLineByteFromIsr_(BYTE const ch)
{
    if (ch == (BYTE)'\n')
    {
        if (isrDropLf == RK_TRUE)
        {
            isrDropLf = RK_FALSE;
            return;
        }

        AppLineSubmitFromIsr_((BYTE)'\n');
        return;
    }

    if (ch == (BYTE)'\r')
    {
        AppLineSubmitFromIsr_((BYTE)'\r');
        isrDropLf = RK_TRUE;
        return;
    }

    isrDropLf = RK_FALSE;

    if (isrLineBytes >= APP_LINE_BYTES)
    {
        return;
    }

    if (AppLineRingWriteFromIsr_(ch, RK_TRUE) == RK_TRUE)
    {
        isrLineBytes++;
    }
}

/*
 * Copy one complete shared-ring line into EchoTask stack storage.
 *
 * EchoTask calls this after it successfully pends on lineReadySemaHandle, so
 * at least one delimiter should already be present. The scan still does not
 * move readPos until the delimiter is found; that preserves a partial line if
 * the task ever observes bytes that arrived before Enter.
 */
static RK_BOOL AppLineSnapshot_(BYTE *const linePtr, ULONG *const bytesPtr)
{
    ULONG scanPos = sharedLineRing.readPos;
    ULONG const writeLimit = sharedLineRing.writePos;
    RK_BOOL gotLine = RK_FALSE;
    ULONG out = 0UL;

    RK_DMB
    while (scanPos != writeLimit)
    {
        BYTE const ch = (BYTE)sharedLineRing.bytes[scanPos];

        scanPos = AppLineRingNext_(scanPos);

        if ((ch == (BYTE)'\r') || (ch == (BYTE)'\n'))
        {
            RK_DMB
            sharedLineRing.readPos = scanPos;
            gotLine = RK_TRUE;
            break;
        }

        if (out < APP_LINE_BYTES)
        {
            linePtr[out] = ch;
            out++;
        }
    }

    *bytesPtr = out;

    return (gotLine);
}

#if defined(RK_MCU_F401RE)
static CHAR const recordSlotPath[RECORD_SLOT_COUNT][6] =
{
    "/rec0",
    "/rec1",
    "/rec2",
    "/rec3"
};

_Static_assert(RECORD_PERSIST_TEXT_BYTES <= RKFS_RECORD_BYTES,
               "Persisted RECORD text must fit one rkfs record");

static CHAR RecordHexNibble_(BYTE value)
{
    value = (BYTE)(value & (BYTE)0x0FU);
    if (value < (BYTE)10U)
    {
        return ((CHAR)('0' + (CHAR)value));
    }

    return ((CHAR)('A' + (CHAR)(value - (BYTE)10U)));
}

static VOID RecordStoreByte_(CHAR *const textPtr,
                             ULONG const pos,
                             BYTE const value)
{
    textPtr[pos] = RecordHexNibble_((BYTE)(value >> 4U));
    textPtr[pos + 1UL] = RecordHexNibble_(value);
}

static VOID RecordToPersistText_(RECORD const *const recordPtr,
                                 CHAR *const textPtr)
{
    RecordStoreByte_(textPtr, 0UL, recordPtr->seq);
    RecordStoreByte_(textPtr, 2UL, recordPtr->nome);
    RecordStoreByte_(textPtr, 4UL, (BYTE)(recordPtr->valor >> 8U));
    RecordStoreByte_(textPtr, 6UL, (BYTE)recordPtr->valor);
    textPtr[8] = '\0';
}

static RK_BOOL RecordHexValue_(CHAR const ch, BYTE *const valuePtr)
{
    BYTE const byte = (BYTE)ch;

    if ((byte >= (BYTE)'0') && (byte <= (BYTE)'9'))
    {
        *valuePtr = (BYTE)(byte - (BYTE)'0');
        return (RK_TRUE);
    }
    if ((byte >= (BYTE)'A') && (byte <= (BYTE)'F'))
    {
        *valuePtr = (BYTE)((byte - (BYTE)'A') + (BYTE)10U);
        return (RK_TRUE);
    }
    if ((byte >= (BYTE)'a') && (byte <= (BYTE)'f'))
    {
        *valuePtr = (BYTE)((byte - (BYTE)'a') + (BYTE)10U);
        return (RK_TRUE);
    }

    return (RK_FALSE);
}

static RK_BOOL RecordLoadByte_(CHAR const *const textPtr,
                               ULONG const pos,
                               BYTE *const valuePtr)
{
    BYTE hi;
    BYTE lo;

    if (RecordHexValue_(textPtr[pos], &hi) != RK_TRUE)
    {
        return (RK_FALSE);
    }
    if (RecordHexValue_(textPtr[pos + 1UL], &lo) != RK_TRUE)
    {
        return (RK_FALSE);
    }

    *valuePtr = (BYTE)((hi << 4U) | lo);
    return (RK_TRUE);
}

static RK_BOOL RecordFromPersistText_(CHAR const *const textPtr,
                                      RECORD *const recordPtr)
{
    BYTE valueHi;
    BYTE valueLo;

    if (RecordLoadByte_(textPtr, 0UL, &recordPtr->seq) != RK_TRUE)
    {
        return (RK_FALSE);
    }
    if (RecordLoadByte_(textPtr, 2UL, &recordPtr->nome) != RK_TRUE)
    {
        return (RK_FALSE);
    }
    if (RecordLoadByte_(textPtr, 4UL, &valueHi) != RK_TRUE)
    {
        return (RK_FALSE);
    }
    if (RecordLoadByte_(textPtr, 6UL, &valueLo) != RK_TRUE)
    {
        return (RK_FALSE);
    }

    recordPtr->valor =
        (USHORT)(((USHORT)valueHi << 8U) | (USHORT)valueLo);
    return (RK_TRUE);
}
#endif

static RK_BOOL RecordNameValid_(BYTE const name)
{
    return (((name >= (BYTE)'!') && (name <= (BYTE)'~') &&
             (AppIsSeparator_(name) != RK_TRUE)) ?
                RK_TRUE :
                RK_FALSE);
}

static RK_BOOL RecordSeqNewer_(BYTE const candidate, BYTE const current)
{
    BYTE const diff = (BYTE)(candidate - current);

    return (((diff != (BYTE)0U) && (diff < (BYTE)128U)) ? RK_TRUE :
                                                               RK_FALSE);
}

static RK_BOOL RecordSeqOlder_(BYTE const candidate, BYTE const current)
{
    return (RecordSeqNewer_(current, candidate));
}

static BYTE RecordNextSeq_(RecordState const *const statePtr)
{
    BYTE newestSeq = 0U;
    RK_BOOL found = RK_FALSE;

    for (ULONG i = 0UL; i < RECORD_SLOT_COUNT; i++)
    {
        if (statePtr->slots[i].valid == 0U)
        {
            continue;
        }
        if ((found != RK_TRUE) ||
            (RecordSeqNewer_(statePtr->slots[i].record.seq,
                             newestSeq) == RK_TRUE))
        {
            newestSeq = statePtr->slots[i].record.seq;
            found = RK_TRUE;
        }
    }

    return ((found == RK_TRUE) ? (BYTE)(newestSeq + (BYTE)1U) : (BYTE)1U);
}

static ULONG RecordOldestSlot_(RecordState const *const statePtr)
{
    ULONG selected = 0UL;

    for (ULONG i = 0UL; i < RECORD_SLOT_COUNT; i++)
    {
        if (statePtr->slots[i].valid == 0U)
        {
            return (i);
        }
    }

    for (ULONG i = 1UL; i < RECORD_SLOT_COUNT; i++)
    {
        if (RecordSeqOlder_(statePtr->slots[i].record.seq,
                            statePtr->slots[selected].record.seq) == RK_TRUE)
        {
            selected = i;
        }
    }

    return (selected);
}

static RK_BOOL RecordFindNewest_(RecordState const *const statePtr,
                                 BYTE const name,
                                 ULONG *const slotPtr)
{
    ULONG selected = 0UL;
    RK_BOOL found = RK_FALSE;

    for (ULONG i = 0UL; i < RECORD_SLOT_COUNT; i++)
    {
        if ((statePtr->slots[i].valid == 0U) ||
            (statePtr->slots[i].record.nome != name))
        {
            continue;
        }

        if ((found != RK_TRUE) ||
            (RecordSeqNewer_(statePtr->slots[i].record.seq,
                             statePtr->slots[selected].record.seq) == RK_TRUE))
        {
            selected = i;
            found = RK_TRUE;
        }
    }

    if (found == RK_TRUE)
    {
        *slotPtr = selected;
    }

    return (found);
}

static RK_ERR RecordStorageWrite_(ULONG const slot,
                                  RECORD const *const recordPtr)
{
#if defined(RK_MCU_F401RE)
    CHAR text[RECORD_PERSIST_TEXT_BYTES];

    RecordToPersistText_(recordPtr, text);
    return (rkFsClientWriteRecord(fsTaskHandle, recordSlotPath[slot], text));
#else
    K_UNUSE(slot);
    K_UNUSE(recordPtr);
    return (RK_ERR_SUCCESS);
#endif
}

static VOID RecordLoad_(RecordState *const statePtr)
{
#if defined(RK_MCU_F401RE)
    for (ULONG i = 0UL; i < RECORD_SLOT_COUNT; i++)
    {
        RKFS_REPLY reply;
        RECORD record;

        RK_MEMSET(&reply, 0, sizeof(reply));
        if (rkFsClientReadRecord(fsTaskHandle,
                                 recordSlotPath[i],
                                 &reply) != RK_ERR_SUCCESS)
        {
            continue;
        }
        if (reply.status != (ULONG)RKFS_STATUS_OK)
        {
            continue;
        }
        if (RecordFromPersistText_(reply.data, &record) != RK_TRUE)
        {
            continue;
        }

        statePtr->slots[i].record = record;
        statePtr->slots[i].valid = 1U;
    }
#endif

    statePtr->nextSeq = RecordNextSeq_(statePtr);
}

static VOID RecordHandleWrite_(RecordState *const statePtr,
                               RecordRequest const *const reqPtr,
                               RecordReply *const replyPtr)
{
    ULONG const slot = RecordOldestSlot_(statePtr);
    RECORD record = reqPtr->record;

    if (RecordNameValid_(record.nome) != RK_TRUE)
    {
        replyPtr->status = (ULONG)RECORD_STATUS_INVALID;
        return;
    }

    record.seq = statePtr->nextSeq;
    if (RecordStorageWrite_(slot, &record) != RK_ERR_SUCCESS)
    {
        replyPtr->status = (ULONG)RECORD_STATUS_STORAGE;
        return;
    }

    statePtr->slots[slot].record = record;
    statePtr->slots[slot].valid = 1U;
    statePtr->nextSeq = (BYTE)(statePtr->nextSeq + (BYTE)1U);

    replyPtr->status = (ULONG)RECORD_STATUS_OK;
    replyPtr->record = record;
}

static VOID RecordHandleRead_(RecordState const *const statePtr,
                              RecordRequest const *const reqPtr,
                              RecordReply *const replyPtr)
{
    ULONG slot;

    if (RecordNameValid_(reqPtr->record.nome) != RK_TRUE)
    {
        replyPtr->status = (ULONG)RECORD_STATUS_INVALID;
        return;
    }

    if (RecordFindNewest_(statePtr, reqPtr->record.nome, &slot) != RK_TRUE)
    {
        replyPtr->status = (ULONG)RECORD_STATUS_NOT_FOUND;
        replyPtr->record.nome = reqPtr->record.nome;
        return;
    }

    replyPtr->status = (ULONG)RECORD_STATUS_OK;
    replyPtr->record = statePtr->slots[slot].record;
}

/*
 * RK01 starts from privileged C runtime, then kInit() calls kApplicationInit(),
 * finalises the MPU layout and dispatches the first ready task. Returning from
 * kInit() would mean the scheduler stopped unexpectedly.
 */
int main(void)
{
    kCoreInit();
    kInit();

    while (1)
    {
        kErrHandler(RK_FAULT_APP_CRASH);
    }
}

/*
 * Application construction runs during RK01 BOOT, before the scheduler starts.
 *
 * Ordering matters:
 *   1. Start the logger before any task can call kLog().
 *   2. Create the module RAM domains.
 *   3. Start the flash filesystem service when this target has one.
 *   4. Start RecordTask and its synchronous-message endpoint.
 *   5. Create the global counting semaphore used by the UART IRQ.
 *   6. Start EchoTask as the console front-end.
 *   7. Enable console RX interrupts after the shared objects exist.
 */
VOID kApplicationInit(VOID)
{
    kLogInit(APP_LOG_PRIO);

    AppCheck_(kModuleInit(&echoModule, echoRam, sizeof(echoRam), "Echo"));
    AppCheck_(kModuleInit(&recordModule, recordRam, sizeof(recordRam), "Rec"));

    recordState = AppCheckPtr_(RK_MODULE_ALLOC(&recordModule, RecordState));
    RK_MEMSET(recordState, 0, sizeof(*recordState));

#if defined(RK_MCU_F401RE)
    AppCheck_(kModuleInit(&fsModule, (BYTE *)&fsRam, sizeof(fsRam), "FS"));
    /*
     * RKFS owns reserved flash and STM32 flash-controller MMIO. Keep callers
     * isolated by exposing it only through copied synchronous messages.
     */
    AppCheck_(kTaskInitPrivileged(&fsTaskHandle, rkFsServerTask, &fsRam,
                                  "FS", fsRam.serverStack, RKFS_STACK_WORDS,
                                  FS_TASK_PRIO, RK_PREEMPT));
    AppCheck_(kSynchMesgInit(fsTaskHandle, sizeof(RKFS_REQUEST)));
#endif

    AppCheck_(kModuleTaskInit(&recordModule, &recordTaskHandle, RecordTask,
                              recordState, "Record", TASK_STACK_WORDS,
                              RECORD_TASK_PRIO, RK_PREEMPT));
    AppCheck_(kSynchMesgInit(recordTaskHandle, sizeof(RecordRequest)));

    AppCheck_(kSemaphoreCreateGlobalScope(&lineReadySemaHandle, 0U,
                                          APP_LINE_READY_MAX));
    AppCheck_(kModuleTaskInit(&echoModule, &echoTaskHandle, EchoTask,
                              RK_NO_ARGS, "Echo", TASK_STACK_WORDS,
                              ECHO_TASK_PRIO, RK_PREEMPT));
    kBoardConsoleRxIsrEnable(AppLineByteFromIsr_);
}

/*
 * Record task.
 *
 * RecordTask is the only owner of RecordState. Other modules use copied
 * synchronous calls, so no caller can keep a raw pointer into the record ring.
 */
VOID RecordTask(VOID *args)
{
    RecordState *const statePtr = (RecordState *)args;

    K_ASSERT(statePtr != NULL);
    if (statePtr == NULL)
    {
        while (1)
        {
            kErrHandler(RK_FAULT_INVALID_PARAM);
        }
    }

    RecordLoad_(statePtr);

    while (1)
    {
        RK_SYNCH_CALL_DATA call;
        RecordRequest req;
        RecordReply reply;
        ULONG reqBytes = 0UL;

        if (kSynchMesgAccept(&call, &req, &reqBytes,
                             RK_WAIT_FOREVER) != RK_ERR_SUCCESS)
        {
            continue;
        }

        RK_MEMSET(&reply, 0, sizeof(reply));
        if (reqBytes != sizeof(req))
        {
            reply.status = (ULONG)RECORD_STATUS_INVALID;
        }
        else
        {
            switch ((RecordCommand)req.command)
            {
                case RECORD_CMD_WRITE:
                    RecordHandleWrite_(statePtr, &req, &reply);
                    break;
                case RECORD_CMD_READ:
                    RecordHandleRead_(statePtr, &req, &reply);
                    break;
                default:
                    reply.status = (ULONG)RECORD_STATUS_INVALID;
                    break;
            }
        }

        AppCheck_(kSynchMesgReply(&call, &reply, sizeof(reply)));
    }
}

/*
 * Console front-end task.
 *
 * EchoTask is unprivileged and isolated in echoModule. It waits on the global
 * counting semaphore, snapshots one complete shared-ring line, parses it into
 * a bounded RecordRequest and sends that request to RecordTask.
 */
VOID EchoTask(VOID *args)
{
    static CHAR const banner[] =
        "\r\nRK01 " APP_CONSOLE_NAME
        " record console ready. SET A 123, READ A.\r\n"
#if defined(RK_MCU_F401RE)
        "Record slots: 4, persisted in flash through rkfs.\r\n";
#else
        "Record slots: 4, RAM-only on this target.\r\n";
#endif
    static CHAR const usage[] =
        "ERR use SET X 123, X=123 or READ X\r\n";
    static CHAR const serviceErr[] = "ERR record service\r\n";
    static CHAR const storageErr[] = "ERR storage\r\n";
    static CHAR const notFound[] = "NOT FOUND ";

    K_UNUSE(args);

    AppConsoleWriteText_(banner);

    while (1)
    {
        BYTE line[APP_LINE_BUF_BYTES];
        ULONG bytes = 0UL;
        RecordRequest req;
        RecordReply reply;
        RK_ERR err;

        AppCheck_(kSemaphorePend(lineReadySemaHandle, RK_WAIT_FOREVER));

        if (AppLineSnapshot_(line, &bytes) != RK_TRUE)
        {
            continue;
        }
        if (bytes == 0UL)
        {
            continue;
        }

        if (AppParseRecordCommand_(line, bytes, &req) != RK_TRUE)
        {
            AppConsoleWriteText_(usage);
            continue;
        }

        RK_MEMSET(&reply, 0, sizeof(reply));
        err = AppRecordCall_(&req, &reply);
        if (err != RK_ERR_SUCCESS)
        {
            AppConsoleWriteText_(serviceErr);
            continue;
        }

        switch ((RecordStatus)reply.status)
        {
            case RECORD_STATUS_OK:
                if (req.command == (ULONG)RECORD_CMD_WRITE)
                {
                    AppConsoleWriteRecord_("STORED", &reply.record);
                }
                else
                {
                    AppConsoleWriteRecord_("READ", &reply.record);
                }
                break;
            case RECORD_STATUS_NOT_FOUND:
            {
                CHAR nameText[3];

                nameText[0] = (CHAR)req.record.nome;
                nameText[1] = '\r';
                nameText[2] = '\n';
                AppConsoleWriteText_(notFound);
                AppCheck_(kConsoleWrite(nameText, sizeof(nameText)));
                break;
            }
            case RECORD_STATUS_STORAGE:
                AppConsoleWriteText_(storageErr);
                break;
            case RECORD_STATUS_INVALID:
            default:
                AppConsoleWriteText_(usage);
                break;
        }
    }
}
