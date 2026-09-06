/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Profiling example 06: minimal same-priority yield context switch test.
 *
 * Conditions matching the RK0 profiling page:
 *   make APP_EXAMPLE=06-profile-ctxsw FPU=OFF EXTRA_DEFS="-DNDEBUG"
 */

#include <kapi_app.h>
#include <kconsole.h>

#ifndef RK_CONF_PROFILE_PENDSV
#define RK_CONF_PROFILE_PENDSV 0
#endif

#define PROFILE_DURATION_MS (10000UL)
#define PROFILE_PENDSV_COMP_CYCLES (7UL)
#define STACKSIZE (128U)

#define RK_PROFILE_DEMCR (*(volatile ULONG *)0xE000EDFCUL)
#define RK_PROFILE_DWT_CTRL (*(volatile ULONG *)0xE0001000UL)
#define RK_PROFILE_DWT_CYCCNT (*(volatile ULONG *)0xE0001004UL)
#define RK_PROFILE_DEMCR_TRCENA (1UL << 24U)
#define RK_PROFILE_DWT_CTRL_CYCCNTENA (1UL << 0U)

#if (RK_CONF_PROFILE_PENDSV == 1)
extern volatile ULONG rkProfilePendSvCycles;
extern volatile ULONG rkProfilePendSvSamples;
#endif

RK_DECLARE_TASK(task1Handle, Task1, stack1, STACKSIZE)
RK_DECLARE_TASK(task2Handle, Task2, stack2, STACKSIZE)
RK_DECLARE_TASK(task3Handle, Task3, stack3, STACKSIZE)

static volatile ULONG counter1;
static volatile ULONG counter2;
static volatile ULONG sampleCount;
static volatile ULONG rawLast;
static volatile ULONG rawMin;
static volatile ULONG rawMax;

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

static VOID ProfileRecordCtxSwitch_(VOID)
{
#if (RK_CONF_PROFILE_PENDSV == 1)
    ULONG const rawCycles = rkProfilePendSvCycles;

    if ((rawCycles == 0UL) || (rkProfilePendSvSamples == 0UL))
    {
        return;
    }

    rawLast = rawCycles;
    if ((sampleCount == 0UL) || (rawCycles < rawMin))
    {
        rawMin = rawCycles;
    }
    if (rawCycles > rawMax)
    {
        rawMax = rawCycles;
    }
    sampleCount++;
#endif
}

static VOID ProfileReport_(RK_TICK const time0, RK_TICK const time1)
{
    ULONG const samples = sampleCount;
    ULONG const last = rawLast;
    ULONG const min = rawMin;
    ULONG const max = rawMax;

    kPuts("\r\nRK01 PROFILE CTXSW ");
    ProfilePrintField_("elapsed_ms=", time1 - time0);
    ProfilePrintField_(" tick_ms=", RK_TICK_INTERVAL_MS);
    ProfilePrintField_(" c1=", counter1);
    ProfilePrintField_(" c2=", counter2);
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

    AppCheck_(kTaskInit(&task1Handle, Task1, RK_NO_ARGS, "Y0", stack1,
                        STACKSIZE, 2U, RK_PREEMPT));
    AppCheck_(kTaskInit(&task2Handle, Task2, RK_NO_ARGS, "Y1", stack2,
                        STACKSIZE, 2U, RK_PREEMPT));
    AppCheck_(kTaskInit(&task3Handle, Task3, RK_NO_ARGS, "REP", stack3,
                        STACKSIZE, 1U, RK_PREEMPT));
}

VOID Task1(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        counter1++;
        kYield();
        ProfileRecordCtxSwitch_();
    }
}

VOID Task2(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        counter2++;
        kYield();
        ProfileRecordCtxSwitch_();
    }
}

VOID Task3(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        RK_TICK const time0 = kTickGetMs();

        AppCheck_(kSleep(RK_MS_TO_TICKS(PROFILE_DURATION_MS)));
        ProfileReport_(time0, kTickGetMs());
    }
}
