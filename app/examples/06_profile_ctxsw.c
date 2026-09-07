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
 * Reports same-domain and inter-domain dispatch classes separately.
 *
 * Conditions matching the RK0 profiling page:
 *   make APP_EXAMPLE=06-profile-ctxsw FPU=OFF EXTRA_DEFS="-DNDEBUG"
 */

#include <kapi_domain.h>
#include <kconsole.h>

#ifndef RK_CONF_PROFILE_PENDSV
#define RK_CONF_PROFILE_PENDSV 0
#endif

#ifndef PROFILE_DURATION_MS
#define PROFILE_DURATION_MS (10000UL)
#endif
#define PROFILE_PENDSV_COMP_CYCLES (7UL)
#define STACKSIZE (128U)
#define DOMAIN_BYTES (1024U)
#define PROFILE_RUN_EVENT RK_EVENT_1
#define PROFILE_PHASE_IDLE (0UL)
#define PROFILE_PHASE_SAME_DOMAIN (1UL)
#define PROFILE_PHASE_INTER_DOMAIN (2UL)

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

RK_DECLARE_TASK(sameTask1Handle, SameTask1, sameStack1, STACKSIZE)
RK_DECLARE_TASK(sameTask2Handle, SameTask2, sameStack2, STACKSIZE)
RK_DECLARE_TASK(reportTaskHandle, ReportTask, reportStack, STACKSIZE)

RK_DECLARE_DOMAIN(crossDomain1, crossDomainRam1, DOMAIN_BYTES)
RK_DECLARE_DOMAIN(crossDomain2, crossDomainRam2, DOMAIN_BYTES)
RK_DECLARE_DOMAIN_TASK(crossTask1Handle, CrossTask1)
RK_DECLARE_DOMAIN_TASK(crossTask2Handle, CrossTask2)
RK_DECLARE_DOMAIN_TASK_STACK(crossStack1, STACKSIZE)
RK_DECLARE_DOMAIN_TASK_STACK(crossStack2, STACKSIZE)

static volatile ULONG profilePhase K_ALIGN(4) RK_SECTION_SHARED_BSS;
static ProfileStats sameStats K_ALIGN(4) RK_SECTION_SHARED_BSS;
static ProfileStats crossStats K_ALIGN(4) RK_SECTION_SHARED_BSS;

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

static ULONG ProfileAdjustedCycles_(ULONG const rawCycles)
{
    return ((rawCycles > PROFILE_PENDSV_COMP_CYCLES)
                ? (rawCycles - PROFILE_PENDSV_COMP_CYCLES)
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

static VOID ProfileRecordCtxSwitch_(ProfileStats *const statsPtr)
{
#if (RK_CONF_PROFILE_PENDSV == 1)
    ULONG const rawCycles = rkProfilePendSvCycles;

    if (rkProfilePendSvSamples == 0UL)
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
#else
    K_UNUSE(statsPtr);
#endif
}

static VOID ProfileReport_(CHAR const *const classNamePtr,
                           ProfileStats const *const statsPtr,
                           RK_TICK const time0,
                           RK_TICK const time1)
{
    ULONG const samples = statsPtr->sampleCount;
    ULONG const last = statsPtr->rawLast;
    ULONG const min = statsPtr->rawMin;
    ULONG const max = statsPtr->rawMax;

    kPuts("\r\nRK01 PROFILE CTXSW ");
    kPuts("class=");
    kPuts(classNamePtr);
    ProfilePrintField_(" elapsed_ms=", time1 - time0);
    ProfilePrintField_(" tick_ms=", RK_TICK_INTERVAL_MS);
    ProfilePrintField_(" c1=", statsPtr->counter1);
    ProfilePrintField_(" c2=", statsPtr->counter2);
    ProfilePrintField_(" samples=", samples);
    ProfilePrintField_(" raw_last=", last);
    ProfilePrintField_(" raw_min=", min);
    ProfilePrintField_(" raw_max=", max);
    ProfilePrintField_(" adj_last=", ProfileAdjustedCycles_(last));
    ProfilePrintField_(" adj_min=", ProfileAdjustedCycles_(min));
    ProfilePrintField_(" adj_max=", ProfileAdjustedCycles_(max));
#if (RK_CONF_PROFILE_PENDSV == 1)
    ProfilePrintField_(" pendsv_samples=", rkProfilePendSvSamples);
#else
    kPuts(" pendsv=disabled");
#endif
    kPuts("\r\n");
}

static VOID ProfileWaitPhase_(ULONG const phase)
{
    while (profilePhase != phase)
    {
        AppCheck_(kEventGet(PROFILE_RUN_EVENT, RK_OPT_EVENT_ANY,
                            NULL, RK_WAIT_FOREVER));
    }
}

static VOID ProfileRunPhase_(ULONG const phase,
                             RK_TASK_HANDLE const task1Handle,
                             RK_TASK_HANDLE const task2Handle,
                             ProfileStats *const statsPtr,
                             CHAR const *const classNamePtr)
{
    RK_TICK time0;
    RK_TICK time1;

    ProfileStatsReset_(statsPtr);
    profilePhase = phase;
    AppCheck_(kEventSet(task1Handle, PROFILE_RUN_EVENT));
    AppCheck_(kEventSet(task2Handle, PROFILE_RUN_EVENT));

    time0 = kTickGetMs();
    AppCheck_(kSleep(RK_MS_TO_TICKS(PROFILE_DURATION_MS)));
    time1 = kTickGetMs();

    profilePhase = PROFILE_PHASE_IDLE;
    ProfileReport_(classNamePtr, statsPtr, time0, time1);
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

    AppCheck_(kDomainInit(&crossDomain1, crossDomainRam1,
                          sizeof(crossDomainRam1), "X0"));
    AppCheck_(kDomainInit(&crossDomain2, crossDomainRam2,
                          sizeof(crossDomainRam2), "X1"));

    AppCheck_(kTaskInit(&sameTask1Handle, SameTask1, RK_NO_ARGS, "S0",
                        sameStack1, STACKSIZE, 2U, RK_PREEMPT));
    AppCheck_(kTaskInit(&sameTask2Handle, SameTask2, RK_NO_ARGS, "S1",
                        sameStack2, STACKSIZE, 2U, RK_PREEMPT));
    AppCheck_(kTaskInitDomain(&crossTask1Handle, CrossTask1, RK_NO_ARGS,
                              "X0", crossStack1, STACKSIZE, 2U, RK_PREEMPT,
                              &crossDomain1));
    AppCheck_(kTaskInitDomain(&crossTask2Handle, CrossTask2, RK_NO_ARGS,
                              "X1", crossStack2, STACKSIZE, 2U, RK_PREEMPT,
                              &crossDomain2));
    AppCheck_(kTaskInit(&reportTaskHandle, ReportTask, RK_NO_ARGS, "REP",
                        reportStack, STACKSIZE, 1U, RK_PREEMPT));
}

VOID SameTask1(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        ProfileWaitPhase_(PROFILE_PHASE_SAME_DOMAIN);
        sameStats.counter1++;
        kYield();
        if (profilePhase == PROFILE_PHASE_SAME_DOMAIN)
        {
            ProfileRecordCtxSwitch_(&sameStats);
        }
    }
}

VOID SameTask2(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        ProfileWaitPhase_(PROFILE_PHASE_SAME_DOMAIN);
        sameStats.counter2++;
        kYield();
        if (profilePhase == PROFILE_PHASE_SAME_DOMAIN)
        {
            ProfileRecordCtxSwitch_(&sameStats);
        }
    }
}

VOID CrossTask1(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        ProfileWaitPhase_(PROFILE_PHASE_INTER_DOMAIN);
        crossStats.counter1++;
        kYield();
        if (profilePhase == PROFILE_PHASE_INTER_DOMAIN)
        {
            ProfileRecordCtxSwitch_(&crossStats);
        }
    }
}

VOID CrossTask2(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        ProfileWaitPhase_(PROFILE_PHASE_INTER_DOMAIN);
        crossStats.counter2++;
        kYield();
        if (profilePhase == PROFILE_PHASE_INTER_DOMAIN)
        {
            ProfileRecordCtxSwitch_(&crossStats);
        }
    }
}

VOID ReportTask(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        ProfileRunPhase_(PROFILE_PHASE_SAME_DOMAIN,
                         sameTask1Handle, sameTask2Handle,
                         &sameStats, "same-domain");
        ProfileRunPhase_(PROFILE_PHASE_INTER_DOMAIN,
                         crossTask1Handle, crossTask2Handle,
                         &crossStats, "inter-domain");
    }
}
