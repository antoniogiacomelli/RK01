/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Profiling example 06: same-priority yield context switch test.
 * Build one dispatch class per binary so the PendSV sample stream is not mixed.
 *
 * Conditions matching the RK0 profiling page:
 *   make APP_EXAMPLE=06-profile-ctxsw FPU=OFF EXTRA_DEFS="-DNDEBUG"
 *
 * Inter-domain mode:
 *   make APP_EXAMPLE=06-profile-ctxsw FPU=OFF \
 *     EXTRA_DEFS="-DNDEBUG -DPROFILE_CTXSW_CLASS=PROFILE_CTXSW_CLASS_INTER_DOMAIN"
 */

#include <kapi_domain.h>
#include <kconsole.h>

#ifndef RK_CONF_PROFILE_PENDSV
#define RK_CONF_PROFILE_PENDSV 0
#endif

#ifndef PROFILE_DURATION_MS
#define PROFILE_DURATION_MS (10000UL)
#endif

#define PROFILE_PENDSV_COMP_EST_CYCLES (7UL)
#define STACKSIZE (128U)
#define DOMAIN_BYTES (1024U)
#define PROFILE_RUN_EVENT RK_EVENT_1
#define PROFILE_PARK_EVENT RK_EVENT_2
#define PROFILE_PHASE_IDLE (0UL)
#define PROFILE_PHASE_ACTIVE (1UL)
#define PROFILE_WORKER_1_MASK (1UL << 0U)
#define PROFILE_WORKER_2_MASK (1UL << 1U)
#define PROFILE_WORKER_ALL_MASK (PROFILE_WORKER_1_MASK | PROFILE_WORKER_2_MASK)
#define PROFILE_CTXSW_CLASS_SAME_DOMAIN (1U)
#define PROFILE_CTXSW_CLASS_INTER_DOMAIN (2U)

#ifndef PROFILE_CTXSW_CLASS
#define PROFILE_CTXSW_CLASS PROFILE_CTXSW_CLASS_SAME_DOMAIN
#endif

#if ((PROFILE_CTXSW_CLASS != PROFILE_CTXSW_CLASS_SAME_DOMAIN) &&             \
     (PROFILE_CTXSW_CLASS != PROFILE_CTXSW_CLASS_INTER_DOMAIN))
#error "PROFILE_CTXSW_CLASS must be PROFILE_CTXSW_CLASS_SAME_DOMAIN or PROFILE_CTXSW_CLASS_INTER_DOMAIN"
#endif

#if (PROFILE_CTXSW_CLASS == PROFILE_CTXSW_CLASS_SAME_DOMAIN)
#define PROFILE_CLASS_NAME "same-domain"
#define PROFILE_TASK_1_NAME "S0"
#define PROFILE_TASK_2_NAME "S1"
#else
#define PROFILE_CLASS_NAME "inter-domain"
#define PROFILE_TASK_1_NAME "X0"
#define PROFILE_TASK_2_NAME "X1"
#endif

#define RK_PROFILE_DEMCR (*(volatile ULONG *)0xE000EDFCUL)
#define RK_PROFILE_DWT_CTRL (*(volatile ULONG *)0xE0001000UL)
#define RK_PROFILE_DWT_CYCCNT (*(volatile ULONG *)0xE0001004UL)
#define RK_PROFILE_DEMCR_TRCENA (1UL << 24U)
#define RK_PROFILE_DWT_CTRL_CYCCNTENA (1UL << 0U)

#if (RK_CONF_PROFILE_PENDSV == 1)
extern volatile ULONG rkProfilePendSvCycles;
extern volatile ULONG rkProfilePendSvSamples;
#endif

typedef struct
{
    volatile ULONG counter1;
    volatile ULONG counter2;
    volatile ULONG sampleCount;
    volatile ULONG rawLast;
    volatile ULONG rawMin;
    volatile ULONG rawMax;
} ProfileStats;

#if (PROFILE_CTXSW_CLASS == PROFILE_CTXSW_CLASS_SAME_DOMAIN)
RK_DECLARE_TASK(profileTask1Handle, ProfileTask1, profileStack1, STACKSIZE)
RK_DECLARE_TASK(profileTask2Handle, ProfileTask2, profileStack2, STACKSIZE)
#else
RK_DECLARE_DOMAIN(crossDomain1, crossDomainRam1, DOMAIN_BYTES)
RK_DECLARE_DOMAIN(crossDomain2, crossDomainRam2, DOMAIN_BYTES)
RK_DECLARE_DOMAIN_TASK(profileTask1Handle, ProfileTask1)
RK_DECLARE_DOMAIN_TASK(profileTask2Handle, ProfileTask2)
RK_DECLARE_DOMAIN_TASK_STACK(profileStack1, STACKSIZE)
RK_DECLARE_DOMAIN_TASK_STACK(profileStack2, STACKSIZE)
#endif
RK_DECLARE_TASK(reportTaskHandle, ReportTask, reportStack, STACKSIZE)

static volatile ULONG profilePhase K_ALIGN(4) RK_SECTION_SHARED_BSS;
static volatile ULONG profileParkMask K_ALIGN(4) RK_SECTION_SHARED_BSS;
static ProfileStats profileStats K_ALIGN(4) RK_SECTION_SHARED_BSS;

static VOID AppCheck_(RK_ERR const err)
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

static VOID ProfileCycleCounterEnable_(VOID)
{
    RK_PROFILE_DEMCR |= RK_PROFILE_DEMCR_TRCENA;
    RK_PROFILE_DWT_CYCCNT = 0UL;
    RK_PROFILE_DWT_CTRL |= RK_PROFILE_DWT_CTRL_CYCCNTENA;
}

static ULONG ProfileEstimatedAdjustedCycles_(ULONG const rawCycles)
{
    return ((rawCycles > PROFILE_PENDSV_COMP_EST_CYCLES)
                ? (rawCycles - PROFILE_PENDSV_COMP_EST_CYCLES)
                : 0UL);
}

static VOID ProfilePrintUL_(ULONG value)
{
    CHAR buf[16];
    UINT pos = (UINT)sizeof(buf);

    buf[--pos] = '\0';
    do
    {
        buf[--pos] = (CHAR)('0' + (value % 10UL));
        value /= 10UL;
    } while ((value != 0UL) && (pos > 0U));

    kPuts(&buf[pos]);
}

static VOID ProfilePrintField_(CHAR const *const namePtr, ULONG const value)
{
    kPuts(namePtr);
    ProfilePrintUL_(value);
}

static VOID ProfileStatsReset_(ProfileStats *const statsPtr)
{
    statsPtr->counter1 = 0UL;
    statsPtr->counter2 = 0UL;
    statsPtr->sampleCount = 0UL;
    statsPtr->rawLast = 0UL;
    statsPtr->rawMin = 0UL;
    statsPtr->rawMax = 0UL;
}

#if (RK_CONF_PROFILE_PENDSV == 1)
static VOID ProfileRecordCtxSwitch_(ProfileStats *const statsPtr,
                                    ULONG const rawCycles,
                                    ULONG const sampleTotal)
{
    if (sampleTotal == 0UL)
    {
        return;
    }

    statsPtr->rawLast = rawCycles;
    if ((statsPtr->sampleCount == 0UL) || (rawCycles < statsPtr->rawMin))
    {
        statsPtr->rawMin = rawCycles;
    }
    if (rawCycles > statsPtr->rawMax)
    {
        statsPtr->rawMax = rawCycles;
    }
    statsPtr->sampleCount++;
}
#endif

static VOID ProfileRecordIfActive_(VOID)
{
#if (RK_CONF_PROFILE_PENDSV == 1)
    ULONG const rawCycles = rkProfilePendSvCycles;
    ULONG const sampleTotal = rkProfilePendSvSamples;

    if (profilePhase == PROFILE_PHASE_ACTIVE)
    {
        ProfileRecordCtxSwitch_(&profileStats, rawCycles, sampleTotal);
    }
#endif
}

static VOID ProfileReport_(CHAR const *const classNamePtr,
                           ProfileStats const *const statsPtr,
                           RK_TICK const time0,
                           RK_TICK const time1,
                           ULONG const pendsvStart,
                           ULONG const pendsvEnd)
{
    ULONG const counter1 = statsPtr->counter1;
    ULONG const counter2 = statsPtr->counter2;
    ULONG const samples = statsPtr->sampleCount;
    ULONG const last = statsPtr->rawLast;
    ULONG const min = statsPtr->rawMin;
    ULONG const max = statsPtr->rawMax;

    kPuts("\r\nRK01 PROFILE CTXSW ");
    kPuts("class=");
    kPuts(classNamePtr);
    ProfilePrintField_(" elapsed_ms=", time1 - time0);
    ProfilePrintField_(" tick_ms=", RK_TICK_INTERVAL_MS);
    ProfilePrintField_(" loops1=", counter1);
    ProfilePrintField_(" loops2=", counter2);
    ProfilePrintField_(" samples=", samples);
    ProfilePrintField_(" raw_last=", last);
    ProfilePrintField_(" raw_min=", min);
    ProfilePrintField_(" raw_max=", max);
    ProfilePrintField_(" est_comp=", PROFILE_PENDSV_COMP_EST_CYCLES);
    ProfilePrintField_(" est_adj_last=",
                       ProfileEstimatedAdjustedCycles_(last));
    ProfilePrintField_(" est_adj_min=",
                       ProfileEstimatedAdjustedCycles_(min));
    ProfilePrintField_(" est_adj_max=",
                       ProfileEstimatedAdjustedCycles_(max));
#if (RK_CONF_PROFILE_PENDSV == 1)
    ProfilePrintField_(" pendsv_delta=", pendsvEnd - pendsvStart);
    ProfilePrintField_(" pendsv_total=", pendsvEnd);
#else
    K_UNUSE(pendsvStart);
    K_UNUSE(pendsvEnd);
    kPuts(" pendsv=disabled");
#endif
    kPuts("\r\n");
}

static VOID ProfileParkWorker_(ULONG const workerMask)
{
    profileParkMask |= workerMask;
    RK_BARRIER
    AppCheck_(kEventSet(reportTaskHandle, PROFILE_PARK_EVENT));
}

static VOID ProfileWaitActive_(ULONG const workerMask)
{
    while (profilePhase != PROFILE_PHASE_ACTIVE)
    {
        ProfileParkWorker_(workerMask);
        AppCheck_(kEventGet(PROFILE_RUN_EVENT, RK_OPT_EVENT_ANY,
                            NULL, RK_WAIT_FOREVER));
    }
}

static VOID ProfileWaitParked_(VOID)
{
    while ((profileParkMask & PROFILE_WORKER_ALL_MASK) !=
           PROFILE_WORKER_ALL_MASK)
    {
        AppCheck_(kEventGet(PROFILE_PARK_EVENT, RK_OPT_EVENT_ANY,
                            NULL, RK_WAIT_FOREVER));
    }
}

static VOID ProfileRunPhase_(VOID)
{
    RK_TICK time0;
    RK_TICK time1;
    ULONG pendsvStart = 0UL;
    ULONG pendsvEnd = 0UL;

    ProfileWaitParked_();
    ProfileStatsReset_(&profileStats);
    profileParkMask = 0UL;
#if (RK_CONF_PROFILE_PENDSV == 1)
    pendsvStart = rkProfilePendSvSamples;
#endif
    profilePhase = PROFILE_PHASE_ACTIVE;
    RK_BARRIER
    AppCheck_(kEventSet(profileTask1Handle, PROFILE_RUN_EVENT));
    AppCheck_(kEventSet(profileTask2Handle, PROFILE_RUN_EVENT));

    time0 = kTickGetMs();
    AppCheck_(kSleep(RK_MS_TO_TICKS(PROFILE_DURATION_MS)));
    time1 = kTickGetMs();
#if (RK_CONF_PROFILE_PENDSV == 1)
    pendsvEnd = rkProfilePendSvSamples;
#endif

    profilePhase = PROFILE_PHASE_IDLE;
    RK_BARRIER
    AppCheck_(kEventSet(profileTask1Handle, PROFILE_RUN_EVENT));
    AppCheck_(kEventSet(profileTask2Handle, PROFILE_RUN_EVENT));
    ProfileWaitParked_();
    ProfileReport_(PROFILE_CLASS_NAME, &profileStats, time0, time1,
                   pendsvStart, pendsvEnd);
}

static VOID ProfileWorkerLoop_(ULONG const workerMask,
                               volatile ULONG *const counterPtr)
{
    while (1)
    {
        ProfileWaitActive_(workerMask);
        (*counterPtr)++;
        kYield();
        ProfileRecordIfActive_();
    }
}

int main(void)
{
    kCoreInit();
    kInit();

    while (1)
    {
        kErrHandler(RK_FAULT_APP_CRASH);
    }
}

VOID kApplicationInit(VOID)
{
    ProfileCycleCounterEnable_();
    AppCheck_(kConsoleServiceInit());

#if (PROFILE_CTXSW_CLASS == PROFILE_CTXSW_CLASS_INTER_DOMAIN)
    AppCheck_(kDomainInit(&crossDomain1, crossDomainRam1,
                          sizeof(crossDomainRam1), "X0"));
    AppCheck_(kDomainInit(&crossDomain2, crossDomainRam2,
                          sizeof(crossDomainRam2), "X1"));

    AppCheck_(kTaskInitDomain(&profileTask1Handle, ProfileTask1, RK_NO_ARGS,
                              PROFILE_TASK_1_NAME, profileStack1, STACKSIZE,
                              2U, RK_PREEMPT, &crossDomain1));
    AppCheck_(kTaskInitDomain(&profileTask2Handle, ProfileTask2, RK_NO_ARGS,
                              PROFILE_TASK_2_NAME, profileStack2, STACKSIZE,
                              2U, RK_PREEMPT, &crossDomain2));
#else
    AppCheck_(kTaskInit(&profileTask1Handle, ProfileTask1, RK_NO_ARGS,
                        PROFILE_TASK_1_NAME, profileStack1, STACKSIZE,
                        2U, RK_PREEMPT));
    AppCheck_(kTaskInit(&profileTask2Handle, ProfileTask2, RK_NO_ARGS,
                        PROFILE_TASK_2_NAME, profileStack2, STACKSIZE,
                        2U, RK_PREEMPT));
#endif
    AppCheck_(kTaskInit(&reportTaskHandle, ReportTask, RK_NO_ARGS, "REP",
                        reportStack, STACKSIZE, 1U, RK_PREEMPT));
}

VOID ProfileTask1(VOID *args)
{
    RK_UNUSEARGS

    ProfileWorkerLoop_(PROFILE_WORKER_1_MASK, &profileStats.counter1);
}

VOID ProfileTask2(VOID *args)
{
    RK_UNUSEARGS

    ProfileWorkerLoop_(PROFILE_WORKER_2_MASK, &profileStats.counter2);
}

VOID ReportTask(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        ProfileRunPhase_();
    }
}
