/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Record domain server for the tiny example.
 *
 * RecordTask is the only owner of RecordState. Console clients send bounded
 * call/reply messages and receive a copied reply.
 */

#include "record_domain_internal.h"
#include "tiny_app.h"

#if (RK_CONF_FILESYSTEM == ON)
#include <rkfs.h>

static CHAR const recordSlotPath[RECORD_SLOT_COUNT][6] = {"/rec0", "/rec1",
                                                          "/rec2", "/rec3"};

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

static VOID RecordStoreByte_(CHAR *const textPtr, ULONG const pos,
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

static RK_BOOL RecordLoadByte_(CHAR const *const textPtr, ULONG const pos,
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

    recordPtr->valor = (USHORT)(((USHORT)valueHi << 8U) | (USHORT)valueLo);
    return (RK_TRUE);
}
#endif

static RK_BOOL RecordNameSeparator_(BYTE const ch)
{
    return (((ch == (BYTE)' ') || (ch == (BYTE)'\t') || (ch == (BYTE)',') ||
             (ch == (BYTE)'='))
                ? RK_TRUE
                : RK_FALSE);
}

static RK_BOOL RecordNameValid_(BYTE const name)
{
    return (((name >= (BYTE)'!') && (name <= (BYTE)'~') &&
             (RecordNameSeparator_(name) != RK_TRUE))
                ? RK_TRUE
                : RK_FALSE);
}

static RK_BOOL RecordSeqNewer_(BYTE const candidate, BYTE const current)
{
    BYTE const diff = (BYTE)(candidate - current);

    return (((diff != (BYTE)0U) && (diff < (BYTE)128U)) ? RK_TRUE : RK_FALSE);
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
            (RecordSeqNewer_(statePtr->slots[i].record.seq, newestSeq) ==
             RK_TRUE))
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
                                 BYTE const name, ULONG *const slotPtr)
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
#if (RK_CONF_FILESYSTEM == ON)
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
#if (RK_CONF_FILESYSTEM == ON)
    for (ULONG i = 0UL; i < RECORD_SLOT_COUNT; i++)
    {
        RKFS_REPLY reply;
        RECORD record;

        RK_MEMSET(&reply, 0, sizeof(reply));
        if (rkFsClientReadRecord(fsTaskHandle, recordSlotPath[i], &reply) !=
            RK_ERR_SUCCESS)
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

VOID RecordTask(VOID *args)
{
    RECORD_DOMAIN_RAM *const ramPtr = (RECORD_DOMAIN_RAM *)args;
    RecordState *const statePtr =
        (ramPtr != NULL) ? &ramPtr->recordState : NULL;

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

        if (kSynchMesgAccept(&call, &req, &reqBytes, RK_WAIT_FOREVER) !=
            RK_ERR_SUCCESS)
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

        {
            RK_ERR err = kSynchMesgReply(&call, &reply, sizeof(reply));
            K_ASSERT(err == RK_ERR_SUCCESS);
        }
    }
}
