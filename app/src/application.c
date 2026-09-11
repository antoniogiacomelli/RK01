/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Default application wiring for the record-console example.
 *
 * The application domain constructs shared infrastructure and boots the source
 * bundled domains. Console parsing and shared helpers live in tiny_console.c;
 * record ownership lives behind record_domain.h.
 */

#include "tiny_app.h"

#include <kapi_trusted.h>
#include <klogger.h>

#if (RK_CONF_FILESYSTEM == ON)
#include <rkfs.h>
#endif

RK_DECLARE_DOMAIN(echoDomain, echoRam, ECHO_DOMAIN_BYTES)
RK_DECLARE_DOMAIN_TASK(echoTaskHandle, EchoTask)
RK_DECLARE_DOMAIN_TASK_STACK(echoStack, TASK_STACK_WORDS)
RK_DECLARE_GLOBAL_SEMAPHORE(lineReadySemaHandle)

#if (RK_CONF_FILESYSTEM == ON)
static RKFS_RAM fsRam RK_RETAINED_TASK_RAM_ATTR;
VOID rkFsServerTask(VOID *args);
RK_DECLARE_GLOBAL_TASK_HANDLE(fsTaskHandle)
RK_DECLARE_DOMAIN_TASK_STACK(fsServerStack, RKFS_STACK_WORDS)
#endif

static RECORD_DOMAIN_EXPORTS recordDomainExports RK_SHARED_RAM_ATTR;
AppLineRing sharedLineRing K_ALIGN(4) RK_SHARED_RAM_ATTR;

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
 *   2. Create the writable domains.
 *   3. Start the flash filesystem service when this target has one.
 *   4. Boot the Record domain and publish its service handle.
 *   5. Create the global counting semaphore used by console RX.
 *   6. Start EchoTask as the console front-end.
 *   7. Claim foreground console RX for EchoTask's line feeder.
 */
VOID kApplicationInit(VOID)
{
    kLogInit(APP_LOG_PRIO);

    {
        RK_ERR err = kDomainInit(&echoDomain, echoRam, sizeof(echoRam), "Echo");
        K_ASSERT(err == RK_ERR_SUCCESS);
    }

#if (RK_CONF_FILESYSTEM == ON)
    /*
     * RKFS is a privileged demonstration service over reserved flash, STM32
     * flash-controller MMIO and reserved service RAM. Its state is not a
     * domain window; callers interact only through copied call/reply.
     */
    {
        RK_ERR err = kTaskInitPrivileged(&fsTaskHandle, rkFsServerTask, &fsRam,
                                         "FS", fsServerStack, RKFS_STACK_WORDS,
                                         FS_TASK_PRIO, RK_PREEMPT);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    {
        RK_ERR err = kSynchMesgInit(fsTaskHandle, sizeof(RKFS_REQUEST));
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
#endif

    {
        RK_ERR err = RecordDomainBoot(&recordDomainExports);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }

    {
        RK_ERR err = kSemaphoreCreateGlobalScope(
            &lineReadySemaHandle, "LineRdy", 0U, APP_LINE_READY_MAX);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    {
        RK_ERR err = kTaskInitDomain(
            &echoTaskHandle, EchoTask, &recordDomainExports, "Echo", echoStack,
            TASK_STACK_WORDS, ECHO_TASK_PRIO, RK_PREEMPT, &echoDomain);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    {
        RK_ERR err = kConsoleRxClaim(AppLineByteFromConsole_);
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
    {
        RK_ERR err = kSysMonInit();
        K_ASSERT(err == RK_ERR_SUCCESS);
    }
}
