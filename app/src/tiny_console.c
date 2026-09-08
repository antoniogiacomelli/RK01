/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Console front-end for the tiny record example.
 *
 * The privileged console driver task writes received bytes into a shared
 * circular buffer. CR, LF and CRLF terminal endings are accepted. When a
 * complete line is registered, the console callback posts a global counting
 * semaphore. EchoTask consumes one semaphore count per complete line, parses
 * console commands and calls the Record domain through copied call/reply
 * messages.
 */

#include "tiny_app.h"

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

static RK_BOOL AppFirstTokenEquals_(BYTE const *const linePtr,
                                    ULONG const lineBytes,
                                    CHAR const *const textPtr,
                                    ULONG *const afterTokenPtr)
{
    ULONG pos = 0UL;
    ULONG tokenStart;
    ULONG tokenBytes;

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

    if (AppTokenEquals_(linePtr, tokenStart, tokenBytes, textPtr) != RK_TRUE)
    {
        return (RK_FALSE);
    }

    if (afterTokenPtr != NULL)
    {
        *afterTokenPtr = pos;
    }
    return (RK_TRUE);
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
        if (value > USHORT_MAX)
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

static RK_BOOL AppLineIsSysMonExit_(BYTE const *const linePtr,
                                    ULONG const lineBytes)
{
    ULONG pos = 0UL;
    RK_BOOL exitLine;

    exitLine =
        ((AppFirstTokenEquals_(linePtr, lineBytes, "exit", &pos) ==
          RK_TRUE) ||
         (AppFirstTokenEquals_(linePtr, lineBytes, "quit", &pos) ==
          RK_TRUE)) ?
            RK_TRUE :
            RK_FALSE;
    if (exitLine != RK_TRUE)
    {
        return (RK_FALSE);
    }

    return (AppOnlySpaces_(linePtr, lineBytes, pos));
}

static RK_ERR AppSubmitSysMonLine_(BYTE const *const linePtr,
                                   ULONG const lineBytes,
                                   RK_BOOL *const sysMonActivePtr)
{
    RK_ERR const err = kSysMonCommand((CHAR const *)linePtr, lineBytes);

    if ((err == RK_ERR_SUCCESS) &&
        (AppLineIsSysMonExit_(linePtr, lineBytes) == RK_TRUE))
    {
        *sysMonActivePtr = RK_FALSE;
    }

    return (err);
}

static RK_ERR AppMaybeSubmitSysMonCommand_(BYTE const *const linePtr,
                                           ULONG const lineBytes,
                                           RK_BOOL *const sysMonActivePtr,
                                           RK_BOOL *const handledPtr)
{
    static CHAR const helpCommand[] = "help";
    ULONG pos;

    *handledPtr = RK_FALSE;

    if (AppFirstTokenEquals_(linePtr, lineBytes, APP_SYSMON_TOKEN,
                             &pos) != RK_TRUE)
    {
        return (RK_ERR_SUCCESS);
    }

    *handledPtr = RK_TRUE;
    *sysMonActivePtr = RK_TRUE;
    AppSkipValueSeparators_(linePtr, lineBytes, &pos);
    if (pos >= lineBytes)
    {
        return (AppSubmitSysMonLine_((BYTE const *)helpCommand,
                                     (ULONG)(sizeof(helpCommand) - 1UL),
                                     sysMonActivePtr));
    }

    return (AppSubmitSysMonLine_(&linePtr[pos], lineBytes - pos,
                                 sysMonActivePtr));
}

static RK_ERR AppRecordCall_(RK_TASK_HANDLE const serviceHandle,
                             RecordRequest *const reqPtr,
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

    if (serviceHandle == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    err = kSynchMesgCall(serviceHandle, &attr, RK_WAIT_FOREVER);
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
 * Compute free bytes from the two positions. If EchoTask advances readPos at
 * the same time, the producer may briefly underestimate free room, which is
 * safe: at worst a byte is dropped even though space just became available.
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
 * Add one byte to the console-service-owned producer side of the ring.
 *
 * Ordinary data bytes reserve one extra slot for the future CR/LF terminator,
 * so a partially typed line cannot consume the last byte needed to publish it.
 */
static RK_BOOL AppLineRingWriteFromConsole_(BYTE const ch,
                                            RK_BOOL const reserveTerm)
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
 * Publish one completed line from the console-service callback.
 *
 * The terminator is a real byte in the shared ring. EchoTask later reads bytes
 * until it sees this terminator.
 */
static VOID AppLineSubmitFromConsole_(BYTE const terminator)
{
    if (AppLineRingWriteFromConsole_(terminator, RK_FALSE) == RK_TRUE)
    {
        if (lineReadySemaHandle != RK_NULL_HANDLE)
        {
            (VOID)kSemaphorePost(lineReadySemaHandle);
        }
    }

    sharedLineRing.lineBytes = 0UL;
}

/*
 * Console RX byte callback, called by the privileged UART driver task.
 *
 * CR, LF and CRLF are accepted as Enter. For CRLF, the CR submits the line and
 * the following LF is consumed by the shared dropLf flag so the terminal does
 * not produce an empty second line.
 */
VOID AppLineByteFromConsole_(BYTE const ch)
{
    if (ch == (BYTE)'\n')
    {
        if (sharedLineRing.dropLf == RK_TRUE)
        {
            sharedLineRing.dropLf = RK_FALSE;
            return;
        }

        AppLineSubmitFromConsole_((BYTE)'\n');
        return;
    }

    if (ch == (BYTE)'\r')
    {
        AppLineSubmitFromConsole_((BYTE)'\r');
        sharedLineRing.dropLf = RK_TRUE;
        return;
    }

    sharedLineRing.dropLf = RK_FALSE;

    if (sharedLineRing.lineBytes >= APP_LINE_BYTES)
    {
        return;
    }

    if (AppLineRingWriteFromConsole_(ch, RK_TRUE) == RK_TRUE)
    {
        sharedLineRing.lineBytes++;
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

VOID EchoTask(VOID *args)
{
    static CHAR const banner[] =
        "\r\nRK01 " APP_CONSOLE_NAME
        " record console ready. SET A 123, READ A, RKMONITOR.\r\n"
#if (RK_CONF_FILESYSTEM == ON)
        "Record slots: 4, persisted in flash through rkfs.\r\n";
#else
        "Record slots: 4, RAM-only on this target.\r\n";
#endif
    static CHAR const usage[] =
        "ERR use SET X 123, X=123, READ X or RKMONITOR\r\n";
    static CHAR const serviceErr[] = "ERR record service\r\n";
    static CHAR const sysMonErr[] = "ERR sysmon\r\n";
    static CHAR const storageErr[] = "ERR storage\r\n";
    static CHAR const notFound[] = "NOT FOUND ";
    RECORD_DOMAIN_EXPORTS const *const recordExportsPtr =
        (RECORD_DOMAIN_EXPORTS const *)args;
    RK_TASK_HANDLE const recordServiceHandle =
        (recordExportsPtr != NULL) ? recordExportsPtr->serviceHandle :
                                     NULL;
    RK_BOOL sysMonActive = RK_FALSE;

    AppConsoleWriteText_(banner);

    while (1)
    {
        BYTE line[APP_LINE_BUF_BYTES];
        ULONG bytes = 0UL;
        RK_BOOL handledBySysMon = RK_FALSE;
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

        if (sysMonActive == RK_TRUE)
        {
            err = AppSubmitSysMonLine_(line, bytes, &sysMonActive);
            if (err != RK_ERR_SUCCESS)
            {
                AppConsoleWriteText_(sysMonErr);
            }
            continue;
        }

        err = AppMaybeSubmitSysMonCommand_(line, bytes, &sysMonActive,
                                           &handledBySysMon);
        if (handledBySysMon == RK_TRUE)
        {
            if (err != RK_ERR_SUCCESS)
            {
                AppConsoleWriteText_(sysMonErr);
            }
            continue;
        }

        if (AppParseRecordCommand_(line, bytes, &req) != RK_TRUE)
        {
            AppConsoleWriteText_(usage);
            continue;
        }

        RK_MEMSET(&reply, 0, sizeof(reply));
        err = AppRecordCall_(recordServiceHandle, &req, &reply);
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
