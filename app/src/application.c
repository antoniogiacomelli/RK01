/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Default application wiring for the record-console example.
 *
 * The application module constructs the service domains and connects console
 * RX to the unprivileged Echo task. Console parsing, record ownership and
 * shared helpers live in the neighbouring tiny_* sources.
 */

#include "tiny_app.h"

#include <kapi_trusted.h>
#include <klogger.h>

#if defined(RK_MCU_F401RE)
#include <rkfs.h>
#endif

RK_DECLARE_MODULE(echoModule, echoRam, ECHO_MODULE_BYTES)
RK_DECLARE_MODULE_TASK(echoTaskHandle, EchoTask)
RK_DECLARE_MODULE(recordModule, recordRam, RECORD_MODULE_BYTES)
RK_DECLARE_MODULE_TASK(recordTaskHandle, RecordTask)
RK_DECLARE_GLOBAL_SEMAPHORE(lineReadySemaHandle)

#if defined(RK_MCU_F401RE)
static RKFS_RAM fsRam RK_MODULE_RAM_ATTR(RKFS_MODULE_BYTES);
static RK_MODULE fsModule RK_MODULE_DESC_ATTR;
RK_DECLARE_MODULE_TASK(fsTaskHandle, rkFsServerTask)
#endif

static RecordState *recordState;
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
 *   2. Create the module RAM domains.
 *   3. Start the flash filesystem service when this target has one.
 *   4. Start RecordTask and its synchronous-message endpoint.
 *   5. Create the global counting semaphore used by console RX.
 *   6. Start EchoTask as the console front-end.
 *   7. Claim foreground console RX for EchoTask's line feeder.
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
     * isolated by exposing it only through copied call/reply.
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

    AppCheck_(kSemaphoreCreateGlobalScope(&lineReadySemaHandle, "LineRdy", 0U,
                                          APP_LINE_READY_MAX));
    AppCheck_(kModuleTaskInit(&echoModule, &echoTaskHandle, EchoTask,
                              RK_NO_ARGS, "Echo", TASK_STACK_WORDS,
                              ECHO_TASK_PRIO, RK_PREEMPT));
    AppCheck_(kConsoleRxClaim(AppLineByteFromConsole_));
    AppCheck_(kSysMonInit());
}
