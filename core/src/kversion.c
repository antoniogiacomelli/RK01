/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * File intent:
 *   Runtime version export. The version is encoded in kversion.h and exposed
 *   through direct privileged calls or the SVC path for unprivileged tasks.
 */

#define RK_SOURCE_CODE

#include <ksyscall.h>
#include <kversion.h>

/* no file system, no NVM map, this is the best we can do */
struct RK_gKversion const RK_gKversion = {RK_VERSION_MAJOR, RK_VERSION_MINOR,
                                          RK_VERSION_PATCH};

unsigned kIsValidVersion(void)
{
    return ((unsigned)((RK_gKversion.major << 16) | (RK_gKversion.minor << 8) |
                       (RK_gKversion.patch << 0)) == RK_VALID_VERSION);
}

unsigned kGetVersion(void)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return ((unsigned)kSyscallInvoke4(RK_SYSCALL_GET_VERSION,
                                          0UL, 0UL, 0UL, 0UL));
    }

    return (RK_VALID_VERSION);
}

void kGetInfo(const char **infoPPtr)
{
    if (infoPPtr != 0)
    {
        *infoPPtr = RK_VER_INFO;
    }
}
