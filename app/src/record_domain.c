/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Record domain construction.
 *
 * The public interface is record_domain.h. The typed RAM layout and task entry
 * stay private to the Record bundle.
 */

#include "record_domain_internal.h"

#include <kstring.h>

RK_DECLARE_TYPED_DOMAIN(recordDomain, recordDomainRam,
                        RECORD_DOMAIN_RAM, RECORD_DOMAIN_BYTES)
RK_DECLARE_DOMAIN_TASK_STACK(recordServerStack, RECORD_TASK_STACK_WORDS)

RK_ERR RecordDomainBoot(RECORD_DOMAIN_EXPORTS *exportsPtr)
{
    RECORD_DOMAIN_RAM *const ramPtr = RK_DOMAIN_STATE(recordDomainRam);
    RK_ERR err;

    if (exportsPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    RK_MEMSET(exportsPtr, 0, sizeof(*exportsPtr));
    RK_MEMSET(RK_DOMAIN_WINDOW_BASE(recordDomainRam), 0,
              RK_DOMAIN_WINDOW_BYTES(recordDomainRam));

    err = RK_DOMAIN_INIT_TYPED(&recordDomain, recordDomainRam, "Rec");
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kTaskInitDomain(&ramPtr->serverHandle, RecordTask, ramPtr,
                          "Record", recordServerStack,
                          RECORD_TASK_STACK_WORDS, RECORD_TASK_PRIO,
                          RK_PREEMPT, &recordDomain);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kSynchMesgInit(ramPtr->serverHandle, sizeof(RecordRequest));
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    exportsPtr->serviceHandle = ramPtr->serverHandle;
    return (RK_ERR_SUCCESS);
}
