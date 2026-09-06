/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * File intent:
 *   Word-oriented circular buffer used by message queues. It performs bounded
 *   copy-in/copy-out operations; synchronisation is owned by the caller.
 *
 * Contracts/invariants:
 *   - readPtr and writePtr always point at element boundaries inside the ring.
 *   - nFull is bounded by maxBuf.
 *   - dataSize is expressed in words, not bytes.
 */

#define RK_SOURCE_CODE

#include <kringbuf.h>

/*
 * Copy one fixed-size ring element expressed in machine words. Message-queue
 * code precomputes dataSize so the ring buffer can stay type-agnostic.
 */
#ifndef K_RINGBUF_CPY
#define K_RINGBUF_CPY(d, s, z)                                                 \
    do                                                                         \
    {                                                                          \
        ULONG words_ = (z);                                                    \
        while (--words_)                                                       \
        {                                                                      \
            *(d)++ = *(s)++;                                                   \
        }                                                                      \
        *(d)++ = *(s)++;                                                       \
    } while (0)
#endif

/* Move one element forward and wrap exactly at the backing-buffer limit. */
static ULONG *kRingBufAdvance_(struct RK_STRUCT_RING_BUFFER const *const kobj,
                               ULONG *ptr)
{
    ptr += kobj->dataSize;
    if (ptr == kobj->bufEndPtr)
    {
        ptr = kobj->bufPtr;
    }
    return (ptr);
}

/* Move one element backward, wrapping from the first element to the last. */
static ULONG *kRingBufRetreat_(struct RK_STRUCT_RING_BUFFER const *const kobj,
                               ULONG *ptr)
{
    if (ptr == kobj->bufPtr)
    {
        ptr = kobj->bufEndPtr;
    }
    ptr -= kobj->dataSize;
    return (ptr);
}

RK_ERR kRingBufInit(struct RK_STRUCT_RING_BUFFER *const kobj,
                    VOID *const bufPtr, ULONG const dataSize,
                    ULONG const maxBuf)
{
    if ((kobj == NULL) || (bufPtr == NULL) || (dataSize == 0UL) ||
        (maxBuf == 0UL))
    {
        return (RK_ERR_INVALID_PARAM);
    }

    kobj->dataSize = dataSize;
    kobj->maxBuf = maxBuf;
    kobj->bufPtr = (ULONG *)bufPtr;
    kobj->bufEndPtr = kobj->bufPtr + (dataSize * maxBuf);
    kRingBufReset(kobj);

    return (RK_ERR_SUCCESS);
}

VOID kRingBufReset(struct RK_STRUCT_RING_BUFFER *const kobj)
{
    kobj->nFull = 0UL;
    kobj->writePtr = kobj->bufPtr;
    kobj->readPtr = kobj->bufPtr;
}

RK_BOOL kRingBufIsEmpty(struct RK_STRUCT_RING_BUFFER const *const kobj)
{
    return ((kobj->nFull == 0UL) ? RK_TRUE : RK_FALSE);
}

RK_BOOL kRingBufIsFull(struct RK_STRUCT_RING_BUFFER const *const kobj)
{
    return ((kobj->nFull >= kobj->maxBuf) ? RK_TRUE : RK_FALSE);
}

VOID kRingBufWrite(struct RK_STRUCT_RING_BUFFER *const kobj,
                   ULONG const *srcPtr)
{
    ULONG *dstPtr = kobj->writePtr;

    K_RINGBUF_CPY(dstPtr, srcPtr, kobj->dataSize);
    kobj->writePtr = kRingBufAdvance_(kobj, kobj->writePtr);
    kobj->nFull++;
}

VOID kRingBufRead(struct RK_STRUCT_RING_BUFFER *const kobj, ULONG *dstPtr)
{
    ULONG *srcPtr = kobj->readPtr;

    K_RINGBUF_CPY(dstPtr, srcPtr, kobj->dataSize);
    kobj->readPtr = kRingBufAdvance_(kobj, kobj->readPtr);
    kobj->nFull--;
}

VOID kRingBufPeek(struct RK_STRUCT_RING_BUFFER const *const kobj, ULONG *dstPtr)
{
    ULONG *srcPtr = kobj->readPtr;

    K_RINGBUF_CPY(dstPtr, srcPtr, kobj->dataSize);
}

VOID kRingBufJam(struct RK_STRUCT_RING_BUFFER *const kobj, ULONG const *srcPtr)
{
    kobj->readPtr = kRingBufRetreat_(kobj, kobj->readPtr);
    {
        ULONG *dstPtr = kobj->readPtr;
        K_RINGBUF_CPY(dstPtr, srcPtr, kobj->dataSize);
    }
    kobj->nFull++;
}

VOID kRingBufOverwrite(struct RK_STRUCT_RING_BUFFER *const kobj,
                       ULONG const *srcPtr)
{
    ULONG *dstPtr = kobj->writePtr;

    K_RINGBUF_CPY(dstPtr, srcPtr, kobj->dataSize);
    kobj->writePtr = kobj->bufPtr;
    kobj->readPtr = kobj->bufPtr;
    kobj->nFull = 1UL;
}
