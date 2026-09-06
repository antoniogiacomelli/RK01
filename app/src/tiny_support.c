/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#include "tiny_app.h"

/*
 * Fail fast during example construction/runtime.
 *
 * Real applications can recover from selected positive outcomes such as a
 * timeout or a full object. This example treats every non-success return as a
 * programming error so object-scope mistakes are immediately visible.
 */
VOID AppCheck_(RK_ERR const err)
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

VOID *AppCheckPtr_(VOID *const ptr)
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

VOID AppConsoleWriteText_(CHAR const *const textPtr)
{
    CHAR const *chunkPtr = textPtr;

    while ((chunkPtr != NULL) && (*chunkPtr != '\0'))
    {
        ULONG const bytes =
            AppTextLen_(chunkPtr, RK_CONSOLE_WRITE_MAX_BYTES);

        if (bytes == 0UL)
        {
            return;
        }

        AppCheck_(kConsoleWrite(chunkPtr, bytes));
        chunkPtr += bytes;
    }
}
