/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#include "tiny_app.h"

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
        ULONG const bytes = AppTextLen_(chunkPtr, RK_CONSOLE_WRITE_MAX_BYTES);

        if (bytes == 0UL)
        {
            return;
        }

        {
            RK_ERR err = kConsoleWrite(chunkPtr, bytes);
            K_ASSERT(err == RK_ERR_SUCCESS);
        }
        chunkPtr += bytes;
    }
}
