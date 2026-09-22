/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                  */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * RK01 F401RE singleton-domain adaptation of the RK0 Thread-Metric benchmark
 * batch. Each benchmark task is created in its own RK domain.
 */

#include <kapi_trusted.h>
#include <kcoredefs.h>
#include <kconsole.h>
#include <stdio.h>

#define RK_TM_BENCH_BASIC (1UL)
#define RK_TM_BENCH_COOPERATIVE (2UL)
#define RK_TM_BENCH_PREEMPTIVE (3UL)
#define RK_TM_BENCH_INTERRUPT (4UL)
#define RK_TM_BENCH_INTERRUPT_PREEMPTION (5UL)
#define RK_TM_BENCH_MESSAGE (6UL)
#define RK_TM_BENCH_SYNCHRONIZATION (7UL)
#define RK_TM_BENCH_MEMORY (8UL)

#ifndef RK_THREAD_METRIC_BENCH
#error "Define RK_THREAD_METRIC_BENCH to one RK_TM_BENCH_* value"
#endif

#ifndef RK_THREAD_METRIC_TEST_DURATION_MS
#define RK_THREAD_METRIC_TEST_DURATION_MS (30000UL)
#endif

#ifndef RK_THREAD_METRIC_CYCLES
#define RK_THREAD_METRIC_CYCLES (0UL)
#endif

#define TM_STACKSIZE (256U)
#define TM_WORKER_PRIO (10U)
#define TM_REPORT_PRIO (2U)
#define TM_DOMAIN_BYTES (32U)
#define TM_BASIC_DOMAIN_BYTES (4096U)
#define TM_SHARED_STATE_ATTR K_ALIGN(4) RK_SECTION_SHARED_BSS

#if (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_BASIC)
#define TM_BENCH_NAME "basic-processing"
#define TM_BENCH_TITLE "Basic Single Thread Processing"
#elif (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_COOPERATIVE)
#define TM_BENCH_NAME "cooperative-scheduling"
#define TM_BENCH_TITLE "Cooperative Scheduling"
#elif (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_PREEMPTIVE)
#define TM_BENCH_NAME "preemptive-scheduling"
#define TM_BENCH_TITLE "Preemptive Scheduling"
#elif (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_INTERRUPT)
#define TM_BENCH_NAME "interrupt-processing"
#define TM_BENCH_TITLE "Interrupt Processing"
#elif (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_INTERRUPT_PREEMPTION)
#define TM_BENCH_NAME "interrupt-preemption-processing"
#define TM_BENCH_TITLE "Interrupt Preemption Processing"
#elif (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_MESSAGE)
#define TM_BENCH_NAME "message-processing"
#define TM_BENCH_TITLE "Message Processing"
#elif (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_SYNCHRONIZATION)
#define TM_BENCH_NAME "synchronization-processing"
#define TM_BENCH_TITLE "Synchronization Processing"
#elif (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_MEMORY)
#define TM_BENCH_NAME "memory-allocation"
#define TM_BENCH_TITLE "Memory Allocation"
#else
#error "Unknown RK_THREAD_METRIC_BENCH value"
#endif

#if (((RK_THREAD_METRIC_BENCH) == RK_TM_BENCH_INTERRUPT) ||                   \
     ((RK_THREAD_METRIC_BENCH) == RK_TM_BENCH_INTERRUPT_PREEMPTION)) &&       \
    (RK_CONF_TRACE == ON)
#error "IRQ Thread-Metric tests require RK_CONF_TRACE=OFF to keep IRQ timing isolated"
#endif

#define TM_DECLARE_SINGLETON_TASK(ID, ENTRY, STACK_WORDS, DOMAIN_BYTES)        \
    RK_DECLARE_DOMAIN(tm##ID##Domain, tm##ID##Ram, DOMAIN_BYTES)              \
    RK_DECLARE_DOMAIN_TASK(tm##ID##Handle, ENTRY)                             \
    RK_DECLARE_DOMAIN_TASK_STACK(tm##ID##Stack, STACK_WORDS)

#define TM_INIT_SINGLETON_TASK(ID, ENTRY, TASK_NAME, DOMAIN_NAME, STACK_WORDS, \
                               PRIORITY)                                      \
    do                                                                        \
    {                                                                         \
        TmCheckErr_(kDomainInit(&tm##ID##Domain, tm##ID##Ram,                 \
                                sizeof(tm##ID##Ram), DOMAIN_NAME),            \
                    DOMAIN_NAME " domain");                                   \
        TmCheckErr_(kTaskInitDomain(&tm##ID##Handle, ENTRY, RK_NO_ARGS,       \
                                    TASK_NAME, tm##ID##Stack, STACK_WORDS,    \
                                    PRIORITY, RK_PREEMPT,                     \
                                    &tm##ID##Domain),                         \
                    TASK_NAME " task");                                       \
    } while (0)

static volatile ULONG tmReportCycles TM_SHARED_STATE_ATTR;
static volatile ULONG tmErrors TM_SHARED_STATE_ATTR;

static VOID TmStopFault_(VOID)
{
    while (1)
    {
        kErrHandler(RK_FAULT_APP_CRASH);
    }
}

static VOID TmCheckErr_(RK_ERR const err, CHAR const *const wherePtr)
{
    if (err != RK_ERR_SUCCESS)
    {
        printf("TM ERR %s %s err=%d\r\n", TM_BENCH_NAME, wherePtr, err);
        tmErrors++;
        K_ASSERT(err == RK_ERR_SUCCESS);
        TmStopFault_();
    }
}

#if (RK_THREAD_METRIC_CYCLES != 0UL)
static VOID TmStopPass_(VOID)
{
    while (1)
    {
        kSleep(RK_MS_TO_TICKS(1000UL));
    }
}
#endif

static RK_TICK TmDurationTicks_(VOID)
{
    RK_TICK ticks = RK_MS_TO_TICKS(RK_THREAD_METRIC_TEST_DURATION_MS);

    if (ticks == 0UL)
    {
        ticks = 1UL;
    }

    return (ticks);
}

#if ((RK_THREAD_METRIC_BENCH != RK_TM_BENCH_COOPERATIVE) &&                  \
     (RK_THREAD_METRIC_BENCH != RK_TM_BENCH_PREEMPTIVE))
static VOID TmReportSimple_(ULONG const relativeMs, ULONG const periodTotal,
                            ULONG const total)
{
    printf("\r\n**** RK01 Thread-Metric %s Test **** Relative Time: %lu ms\r\n",
           TM_BENCH_TITLE, relativeMs);
    printf("Time Period Total: %lu\r\n", periodTotal);
    printf("Total: %lu errors=%lu tick_ms=%lu RK_CONF_SYSCORECLK=%lu "
           "RK_gSysCoreClock=%lu\r\n",
           total, tmErrors, RK_TICK_INTERVAL_MS, (ULONG)RK_CONF_SYSCORECLK,
           (ULONG)RK_gSysCoreClock);
}
#endif

static VOID TmAfterReport_(VOID)
{
    tmReportCycles++;

#if (RK_THREAD_METRIC_CYCLES != 0UL)
    if (tmReportCycles >= (ULONG)RK_THREAD_METRIC_CYCLES)
    {
        if (tmErrors == 0UL)
        {
            printf("TM PASS %s cycles=%lu errors=%lu\r\n", TM_BENCH_NAME,
                   tmReportCycles, tmErrors);
        }
        else
        {
            printf("TM FAIL %s cycles=%lu errors=%lu\r\n", TM_BENCH_NAME,
                   tmReportCycles, tmErrors);
        }
        TmStopPass_();
    }
#endif
}

static VOID TmBootReport_(VOID)
{
    printf("TM BOOT %s class=singleton-domain tick_ms=%lu "
           "RK_CONF_SYSCORECLK=%lu RK_gSysCoreClock=%lu\r\n",
           TM_BENCH_NAME, RK_TICK_INTERVAL_MS, (ULONG)RK_CONF_SYSCORECLK,
           (ULONG)RK_gSysCoreClock);
}

int main(void)
{
    kCoreInit();
    TmBootReport_();
    kInit();

    TmStopFault_();
}

#if ((RK_THREAD_METRIC_BENCH == RK_TM_BENCH_COOPERATIVE) ||                   \
     (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_PREEMPTIVE))
static ULONG TmAbsDelta_(ULONG const value, ULONG const average)
{
    return ((value >= average) ? (value - average) : (average - value));
}

static VOID TmReportFiveCounters_(ULONG const relativeMs,
                                  ULONG const periodTotal,
                                  ULONG const c0, ULONG const c1,
                                  ULONG const c2, ULONG const c3,
                                  ULONG const c4)
{
    ULONG const total = c0 + c1 + c2 + c3 + c4;
    ULONG const average = total / 5UL;
    ULONG maxDelta = TmAbsDelta_(c0, average);
    ULONG delta = TmAbsDelta_(c1, average);

    if (delta > maxDelta)
    {
        maxDelta = delta;
    }
    delta = TmAbsDelta_(c2, average);
    if (delta > maxDelta)
    {
        maxDelta = delta;
    }
    delta = TmAbsDelta_(c3, average);
    if (delta > maxDelta)
    {
        maxDelta = delta;
    }
    delta = TmAbsDelta_(c4, average);
    if (delta > maxDelta)
    {
        maxDelta = delta;
    }

    if (maxDelta > 1UL)
    {
        printf("TM FAIL %s counter skew average=%lu max_delta=%lu\r\n",
               TM_BENCH_NAME, average, maxDelta);
        tmErrors++;
    }

    printf("\r\n**** RK01 Thread-Metric %s Test **** Relative Time: %lu ms\r\n",
           TM_BENCH_TITLE, relativeMs);
    printf("Time Period Total: %lu\r\n", periodTotal);
    printf("Counters: %lu %lu %lu %lu %lu total=%lu average=%lu "
           "max_delta=%lu errors=%lu tick_ms=%lu RK_CONF_SYSCORECLK=%lu "
           "RK_gSysCoreClock=%lu\r\n",
           c0, c1, c2, c3, c4, total, average, maxDelta, tmErrors,
           RK_TICK_INTERVAL_MS, (ULONG)RK_CONF_SYSCORECLK,
           (ULONG)RK_gSysCoreClock);
}
#endif

#if ((RK_THREAD_METRIC_BENCH == RK_TM_BENCH_PREEMPTIVE) ||                    \
     (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_INTERRUPT_PREEMPTION))
#define TmSuspendSelf_ kTaskSelfSuspend
#define TmResumeTask_ kTaskResume
#endif

#if ((RK_THREAD_METRIC_BENCH == RK_TM_BENCH_INTERRUPT) ||                     \
     (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_INTERRUPT_PREEMPTION))
#if !defined(RK_MCU_F401RE)
#error "This benchmark port is F401RE-only"
#endif

#define TM_IRQ_NUM (0UL)
#define TM_IRQ_HANDLER GPIO_Handler
#define TM_IRQ_REG_INDEX (TM_IRQ_NUM >> 5U)
#define TM_IRQ_BIT (1UL << (TM_IRQ_NUM & 31UL))
#define TM_NVIC_ISER(index)                                                   \
    (*(volatile ULONG *)(0xE000E100UL + (4UL * (index))))
#define TM_NVIC_ISPR(index)                                                   \
    (*(volatile ULONG *)(0xE000E200UL + (4UL * (index))))
#define TM_NVIC_ICPR(index)                                                   \
    (*(volatile ULONG *)(0xE000E280UL + (4UL * (index))))

static volatile ULONG tmIrqCount TM_SHARED_STATE_ATTR;
static volatile ULONG tmIrqErrors TM_SHARED_STATE_ATTR;

static VOID TmIrqInit_(VOID)
{
    TM_NVIC_ICPR(TM_IRQ_REG_INDEX) = TM_IRQ_BIT;
    TM_NVIC_ISER(TM_IRQ_REG_INDEX) = TM_IRQ_BIT;
    RK_DSB
    RK_ISB
}

static VOID TmCauseInterrupt_(VOID)
{
    TM_NVIC_ISPR(TM_IRQ_REG_INDEX) = TM_IRQ_BIT;
    RK_DSB
    RK_ISB
}
#endif

#if (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_BASIC)
typedef struct
{
    volatile ULONG scratch[1024];
} TmBasicDomainState;

RK_DECLARE_TYPED_DOMAIN(tmWorkerDomain, tmWorkerRam, TmBasicDomainState,
                        TM_BASIC_DOMAIN_BYTES)
RK_DECLARE_DOMAIN_TASK(tmWorkerHandle, TmBasicWorker)
RK_DECLARE_DOMAIN_TASK_STACK(tmWorkerStack, TM_STACKSIZE)
TM_DECLARE_SINGLETON_TASK(Report, TmBasicReport, TM_STACKSIZE, TM_DOMAIN_BYTES)

static volatile ULONG tmBasicCounter TM_SHARED_STATE_ATTR;

VOID kApplicationInit(VOID)
{
    TmCheckErr_(kConsoleServiceInit(), "console service");
    TmCheckErr_(RK_DOMAIN_INIT_TYPED(&tmWorkerDomain, tmWorkerRam, "B0D"),
                "B0D domain");
    TmCheckErr_(kTaskInitDomain(&tmWorkerHandle, TmBasicWorker, RK_NO_ARGS,
                                "TMB0", tmWorkerStack, TM_STACKSIZE,
                                TM_WORKER_PRIO, RK_PREEMPT,
                                &tmWorkerDomain),
                "TMB0 task");
    TM_INIT_SINGLETON_TASK(Report, TmBasicReport, "TMR", "RPD",
                           TM_STACKSIZE, TM_REPORT_PRIO);
}

VOID TmBasicWorker(VOID *args)
{
    TmBasicDomainState *const statePtr = RK_DOMAIN_STATE(tmWorkerRam);

    RK_UNUSEARGS

    for (UINT i = 0U; i < 1024U; i++)
    {
        statePtr->scratch[i] = 0UL;
    }

    while (1)
    {
        for (UINT i = 0U; i < 1024U; i++)
        {
            statePtr->scratch[i] =
                (statePtr->scratch[i] + tmBasicCounter) ^
                statePtr->scratch[i];
        }

        tmBasicCounter++;
    }
}

VOID TmBasicReport(VOID *args)
{
    ULONG lastCounter = 0UL;
    ULONG relativeMs = 0UL;

    RK_UNUSEARGS

    while (1)
    {
        kSleep(TmDurationTicks_());
        relativeMs += RK_THREAD_METRIC_TEST_DURATION_MS;

        ULONG const counter = tmBasicCounter;
        if (counter == lastCounter)
        {
            printf("TM FAIL %s worker counter did not move\r\n",
                   TM_BENCH_NAME);
            tmErrors++;
        }

        TmReportSimple_(relativeMs, counter - lastCounter, counter);
        lastCounter = counter;
        TmAfterReport_();
    }
}
#endif

#if (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_COOPERATIVE)
TM_DECLARE_SINGLETON_TASK(Task0, TmCoop0, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Task1, TmCoop1, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Task2, TmCoop2, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Task3, TmCoop3, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Task4, TmCoop4, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Report, TmCoopReport, TM_STACKSIZE, TM_DOMAIN_BYTES)

static volatile ULONG tmCoopCounter0 TM_SHARED_STATE_ATTR;
static volatile ULONG tmCoopCounter1 TM_SHARED_STATE_ATTR;
static volatile ULONG tmCoopCounter2 TM_SHARED_STATE_ATTR;
static volatile ULONG tmCoopCounter3 TM_SHARED_STATE_ATTR;
static volatile ULONG tmCoopCounter4 TM_SHARED_STATE_ATTR;

VOID kApplicationInit(VOID)
{
    TmCheckErr_(kConsoleServiceInit(), "console service");
    TM_INIT_SINGLETON_TASK(Task0, TmCoop0, "TMC0", "C0D", TM_STACKSIZE,
                           TM_WORKER_PRIO);
    TM_INIT_SINGLETON_TASK(Task1, TmCoop1, "TMC1", "C1D", TM_STACKSIZE,
                           TM_WORKER_PRIO);
    TM_INIT_SINGLETON_TASK(Task2, TmCoop2, "TMC2", "C2D", TM_STACKSIZE,
                           TM_WORKER_PRIO);
    TM_INIT_SINGLETON_TASK(Task3, TmCoop3, "TMC3", "C3D", TM_STACKSIZE,
                           TM_WORKER_PRIO);
    TM_INIT_SINGLETON_TASK(Task4, TmCoop4, "TMC4", "C4D", TM_STACKSIZE,
                           TM_WORKER_PRIO);
    TM_INIT_SINGLETON_TASK(Report, TmCoopReport, "TMR", "RPD",
                           TM_STACKSIZE, TM_REPORT_PRIO);
}

VOID TmCoop0(VOID *args)
{
    RK_UNUSEARGS
    while (1)
    {
        tmCoopCounter0++;
        kYield();
    }
}

VOID TmCoop1(VOID *args)
{
    RK_UNUSEARGS
    while (1)
    {
        tmCoopCounter1++;
        kYield();
    }
}

VOID TmCoop2(VOID *args)
{
    RK_UNUSEARGS
    while (1)
    {
        tmCoopCounter2++;
        kYield();
    }
}

VOID TmCoop3(VOID *args)
{
    RK_UNUSEARGS
    while (1)
    {
        tmCoopCounter3++;
        kYield();
    }
}

VOID TmCoop4(VOID *args)
{
    RK_UNUSEARGS
    while (1)
    {
        tmCoopCounter4++;
        kYield();
    }
}

VOID TmCoopReport(VOID *args)
{
    ULONG lastTotal = 0UL;
    ULONG relativeMs = 0UL;

    RK_UNUSEARGS

    while (1)
    {
        kSleep(TmDurationTicks_());
        relativeMs += RK_THREAD_METRIC_TEST_DURATION_MS;

        ULONG const c0 = tmCoopCounter0;
        ULONG const c1 = tmCoopCounter1;
        ULONG const c2 = tmCoopCounter2;
        ULONG const c3 = tmCoopCounter3;
        ULONG const c4 = tmCoopCounter4;
        ULONG const total = c0 + c1 + c2 + c3 + c4;

        TmReportFiveCounters_(relativeMs, total - lastTotal, c0, c1, c2,
                              c3, c4);
        lastTotal = total;
        TmAfterReport_();
    }
}
#endif

#if (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_PREEMPTIVE)
TM_DECLARE_SINGLETON_TASK(Task0, TmPreempt0, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Task1, TmPreempt1, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Task2, TmPreempt2, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Task3, TmPreempt3, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Task4, TmPreempt4, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Report, TmPreemptReport, TM_STACKSIZE,
                          TM_DOMAIN_BYTES)

static volatile ULONG tmPreemptCounter0 TM_SHARED_STATE_ATTR;
static volatile ULONG tmPreemptCounter1 TM_SHARED_STATE_ATTR;
static volatile ULONG tmPreemptCounter2 TM_SHARED_STATE_ATTR;
static volatile ULONG tmPreemptCounter3 TM_SHARED_STATE_ATTR;
static volatile ULONG tmPreemptCounter4 TM_SHARED_STATE_ATTR;

VOID kApplicationInit(VOID)
{
    TmCheckErr_(kConsoleServiceInit(), "console service");
    TM_INIT_SINGLETON_TASK(Task0, TmPreempt0, "TMP0", "P0D", TM_STACKSIZE,
                           6U);
    TM_INIT_SINGLETON_TASK(Task1, TmPreempt1, "TMP1", "P1D", TM_STACKSIZE,
                           5U);
    TM_INIT_SINGLETON_TASK(Task2, TmPreempt2, "TMP2", "P2D", TM_STACKSIZE,
                           4U);
    TM_INIT_SINGLETON_TASK(Task3, TmPreempt3, "TMP3", "P3D", TM_STACKSIZE,
                           3U);
    TM_INIT_SINGLETON_TASK(Task4, TmPreempt4, "TMP4", "P4D", TM_STACKSIZE,
                           2U);
    TM_INIT_SINGLETON_TASK(Report, TmPreemptReport, "TMR", "RPD",
                           TM_STACKSIZE, 1U);
}

VOID TmPreempt0(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        TmResumeTask_(tmTask1Handle);
        tmPreemptCounter0++;
    }
}

VOID TmPreempt1(VOID *args)
{
    RK_UNUSEARGS

    TmSuspendSelf_();

    while (1)
    {
        TmResumeTask_(tmTask2Handle);
        tmPreemptCounter1++;
        TmSuspendSelf_();
    }
}

VOID TmPreempt2(VOID *args)
{
    RK_UNUSEARGS

    TmSuspendSelf_();

    while (1)
    {
        TmResumeTask_(tmTask3Handle);
        tmPreemptCounter2++;
        TmSuspendSelf_();
    }
}

VOID TmPreempt3(VOID *args)
{
    RK_UNUSEARGS

    TmSuspendSelf_();

    while (1)
    {
        TmResumeTask_(tmTask4Handle);
        tmPreemptCounter3++;
        TmSuspendSelf_();
    }
}

VOID TmPreempt4(VOID *args)
{
    RK_UNUSEARGS

    TmSuspendSelf_();

    while (1)
    {
        tmPreemptCounter4++;
        TmSuspendSelf_();
    }
}

VOID TmPreemptReport(VOID *args)
{
    ULONG lastTotal = 0UL;
    ULONG relativeMs = 0UL;

    RK_UNUSEARGS

    while (1)
    {
        kSleep(TmDurationTicks_());
        relativeMs += RK_THREAD_METRIC_TEST_DURATION_MS;

        ULONG const c0 = tmPreemptCounter0;
        ULONG const c1 = tmPreemptCounter1;
        ULONG const c2 = tmPreemptCounter2;
        ULONG const c3 = tmPreemptCounter3;
        ULONG const c4 = tmPreemptCounter4;
        ULONG const total = c0 + c1 + c2 + c3 + c4;

        TmReportFiveCounters_(relativeMs, total - lastTotal, c0, c1, c2,
                              c3, c4);
        lastTotal = total;
        TmAfterReport_();
    }
}
#endif

#if (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_INTERRUPT)
TM_DECLARE_SINGLETON_TASK(Worker, TmInterruptWorker, TM_STACKSIZE,
                          TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Report, TmInterruptReport, TM_STACKSIZE,
                          TM_DOMAIN_BYTES)

static RK_SEMAPHORE_HANDLE tmInterruptSemaHandle TM_SHARED_STATE_ATTR =
    RK_NULL_HANDLE;
static volatile ULONG tmInterruptCounter TM_SHARED_STATE_ATTR;

VOID TM_IRQ_HANDLER(void)
{
    TM_NVIC_ICPR(TM_IRQ_REG_INDEX) = TM_IRQ_BIT;
    tmIrqCount++;
    if (kSemaphorePost(tmInterruptSemaHandle) != RK_ERR_SUCCESS)
    {
        tmIrqErrors++;
    }
}

VOID kApplicationInit(VOID)
{
    TmCheckErr_(kConsoleServiceInit(), "console service");
    TmCheckErr_(kSemaphoreCreateGlobalScope(&tmInterruptSemaHandle, "TMI",
                                            0U, 1U),
                "interrupt sema");
    TmIrqInit_();
    TM_INIT_SINGLETON_TASK(Worker, TmInterruptWorker, "TMI0", "I0D",
                           TM_STACKSIZE, TM_WORKER_PRIO);
    TM_INIT_SINGLETON_TASK(Report, TmInterruptReport, "TMR", "RPD",
                           TM_STACKSIZE, TM_REPORT_PRIO);
}

VOID TmInterruptWorker(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        TmCauseInterrupt_();
        TmCheckErr_(kSemaphorePend(tmInterruptSemaHandle, RK_WAIT_FOREVER),
                    "interrupt sema pend");
        tmInterruptCounter++;
    }
}

VOID TmInterruptReport(VOID *args)
{
    ULONG lastCounter = 0UL;
    ULONG lastIrq = 0UL;
    ULONG relativeMs = 0UL;

    RK_UNUSEARGS

    while (1)
    {
        kSleep(TmDurationTicks_());
        relativeMs += RK_THREAD_METRIC_TEST_DURATION_MS;

        ULONG const counter = tmInterruptCounter;
        ULONG const irqCounter = tmIrqCount;
        if ((counter == lastCounter) || (irqCounter == lastIrq) ||
            (tmIrqErrors != 0UL))
        {
            printf("TM FAIL %s no progress or irq error irq_errors=%lu\r\n",
                   TM_BENCH_NAME, tmIrqErrors);
            tmErrors++;
        }

        TmReportSimple_(relativeMs, counter - lastCounter, counter);
        printf("Interrupts: total=%lu period=%lu irq_errors=%lu\r\n",
               irqCounter, irqCounter - lastIrq, tmIrqErrors);
        lastCounter = counter;
        lastIrq = irqCounter;
        TmAfterReport_();
    }
}
#endif

#if (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_INTERRUPT_PREEMPTION)
TM_DECLARE_SINGLETON_TASK(Low, TmIrqPreemptLow, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(High, TmIrqPreemptHigh, TM_STACKSIZE,
                          TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Report, TmIrqPreemptReport, TM_STACKSIZE,
                          TM_DOMAIN_BYTES)

static volatile ULONG tmIrqPreemptLowCounter TM_SHARED_STATE_ATTR;
static volatile ULONG tmIrqPreemptHighCounter TM_SHARED_STATE_ATTR;

VOID TM_IRQ_HANDLER(void)
{
    TM_NVIC_ICPR(TM_IRQ_REG_INDEX) = TM_IRQ_BIT;
    tmIrqCount++;
    if (TmResumeTask_(tmHighHandle) != RK_ERR_SUCCESS)
    {
        tmIrqErrors++;
    }
}

VOID kApplicationInit(VOID)
{
    TmCheckErr_(kConsoleServiceInit(), "console service");
    TmIrqInit_();
    TM_INIT_SINGLETON_TASK(Low, TmIrqPreemptLow, "TMIL", "ILD",
                           TM_STACKSIZE, 10U);
    TM_INIT_SINGLETON_TASK(High, TmIrqPreemptHigh, "TMIH", "IHD",
                           TM_STACKSIZE, 5U);
    TM_INIT_SINGLETON_TASK(Report, TmIrqPreemptReport, "TMR", "RPD",
                           TM_STACKSIZE, TM_REPORT_PRIO);
}

VOID TmIrqPreemptLow(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        TmCauseInterrupt_();
        tmIrqPreemptLowCounter++;
    }
}

VOID TmIrqPreemptHigh(VOID *args)
{
    RK_UNUSEARGS

    TmSuspendSelf_();

    while (1)
    {
        tmIrqPreemptHighCounter++;
        TmSuspendSelf_();
    }
}

VOID TmIrqPreemptReport(VOID *args)
{
    ULONG lastHigh = 0UL;
    ULONG lastLow = 0UL;
    ULONG lastIrq = 0UL;
    ULONG relativeMs = 0UL;

    RK_UNUSEARGS

    while (1)
    {
        kSleep(TmDurationTicks_());
        relativeMs += RK_THREAD_METRIC_TEST_DURATION_MS;

        ULONG const high = tmIrqPreemptHighCounter;
        ULONG const low = tmIrqPreemptLowCounter;
        ULONG const irqCounter = tmIrqCount;
        if ((high == lastHigh) || (low == lastLow) ||
            (irqCounter == lastIrq) || (tmIrqErrors != 0UL))
        {
            printf("TM FAIL %s no progress or irq error irq_errors=%lu\r\n",
                   TM_BENCH_NAME, tmIrqErrors);
            tmErrors++;
        }

        TmReportSimple_(relativeMs, high - lastHigh, high);
        printf("Low thread: total=%lu period=%lu\r\n", low, low - lastLow);
        printf("Interrupts: total=%lu period=%lu irq_errors=%lu\r\n",
               irqCounter, irqCounter - lastIrq, tmIrqErrors);
        lastHigh = high;
        lastLow = low;
        lastIrq = irqCounter;
        TmAfterReport_();
    }
}
#endif

#if (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_MESSAGE)
TM_DECLARE_SINGLETON_TASK(Worker, TmMessageWorker, TM_STACKSIZE,
                          TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Report, TmMessageReport, TM_STACKSIZE,
                          TM_DOMAIN_BYTES)

static RK_MESG_QUEUE_HANDLE tmMessageQueueHandle TM_SHARED_STATE_ATTR =
    RK_NULL_HANDLE;
static ULONG tmMessageQueueBuf[4] K_ALIGN(4);
static volatile ULONG tmMessageCounter TM_SHARED_STATE_ATTR;

VOID kApplicationInit(VOID)
{
    TmCheckErr_(kConsoleServiceInit(), "console service");
    TmCheckErr_(kMesgQueueCreateGlobalScope(&tmMessageQueueHandle, "TMQ",
                                            tmMessageQueueBuf, 4UL, 1UL),
                "message queue");
    TM_INIT_SINGLETON_TASK(Worker, TmMessageWorker, "TMM0", "M0D",
                           TM_STACKSIZE, TM_WORKER_PRIO);
    TM_INIT_SINGLETON_TASK(Report, TmMessageReport, "TMR", "RPD",
                           TM_STACKSIZE, TM_REPORT_PRIO);
}

VOID TmMessageWorker(VOID *args)
{
    ULONG send[4] = {0x01020304UL, 0x11121314UL, 0x21222324UL,
                     0x31323334UL};
    ULONG recv[4] = {0UL, 0UL, 0UL, 0UL};

    RK_UNUSEARGS

    while (1)
    {
        send[0] = tmMessageCounter;
        TmCheckErr_(kMesgQueueSend(tmMessageQueueHandle, send, RK_NO_WAIT),
                    "message send");
        TmCheckErr_(kMesgQueueRecv(tmMessageQueueHandle, recv, RK_NO_WAIT),
                    "message receive");
        tmMessageCounter++;
    }
}

VOID TmMessageReport(VOID *args)
{
    ULONG lastCounter = 0UL;
    ULONG relativeMs = 0UL;

    RK_UNUSEARGS

    while (1)
    {
        kSleep(TmDurationTicks_());
        relativeMs += RK_THREAD_METRIC_TEST_DURATION_MS;

        ULONG const counter = tmMessageCounter;
        if (counter == lastCounter)
        {
            printf("TM FAIL %s worker counter did not move\r\n",
                   TM_BENCH_NAME);
            tmErrors++;
        }

        TmReportSimple_(relativeMs, counter - lastCounter, counter);
        lastCounter = counter;
        TmAfterReport_();
    }
}
#endif

#if (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_SYNCHRONIZATION)
TM_DECLARE_SINGLETON_TASK(Worker, TmSyncWorker, TM_STACKSIZE, TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Report, TmSyncReport, TM_STACKSIZE, TM_DOMAIN_BYTES)

static RK_SEMAPHORE_HANDLE tmSyncSemaHandle TM_SHARED_STATE_ATTR =
    RK_NULL_HANDLE;
static volatile ULONG tmSyncCounter TM_SHARED_STATE_ATTR;

VOID kApplicationInit(VOID)
{
    TmCheckErr_(kConsoleServiceInit(), "console service");
    TmCheckErr_(kSemaphoreCreateGlobalScope(&tmSyncSemaHandle, "TMS", 1U, 1U),
                "sync sema");
    TM_INIT_SINGLETON_TASK(Worker, TmSyncWorker, "TMS0", "S0D",
                           TM_STACKSIZE, TM_WORKER_PRIO);
    TM_INIT_SINGLETON_TASK(Report, TmSyncReport, "TMR", "RPD",
                           TM_STACKSIZE, TM_REPORT_PRIO);
}

VOID TmSyncWorker(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        TmCheckErr_(kSemaphorePend(tmSyncSemaHandle, RK_WAIT_FOREVER),
                    "sync sema pend");
        TmCheckErr_(kSemaphorePost(tmSyncSemaHandle), "sync sema post");
        tmSyncCounter++;
    }
}

VOID TmSyncReport(VOID *args)
{
    ULONG lastCounter = 0UL;
    ULONG relativeMs = 0UL;

    RK_UNUSEARGS

    while (1)
    {
        kSleep(TmDurationTicks_());
        relativeMs += RK_THREAD_METRIC_TEST_DURATION_MS;

        ULONG const counter = tmSyncCounter;
        if (counter == lastCounter)
        {
            printf("TM FAIL %s worker counter did not move\r\n",
                   TM_BENCH_NAME);
            tmErrors++;
        }

        TmReportSimple_(relativeMs, counter - lastCounter, counter);
        lastCounter = counter;
        TmAfterReport_();
    }
}
#endif

#if (RK_THREAD_METRIC_BENCH == RK_TM_BENCH_MEMORY)
TM_DECLARE_SINGLETON_TASK(Worker, TmMemoryWorker, TM_STACKSIZE,
                          TM_DOMAIN_BYTES)
TM_DECLARE_SINGLETON_TASK(Report, TmMemoryReport, TM_STACKSIZE,
                          TM_DOMAIN_BYTES)

static RK_MEM_PARTITION tmMemoryPool TM_SHARED_STATE_ATTR;
static BYTE tmMemoryPoolBuf[128] TM_SHARED_STATE_ATTR;
static volatile ULONG tmMemoryCounter TM_SHARED_STATE_ATTR;

VOID kApplicationInit(VOID)
{
    TmCheckErr_(kConsoleServiceInit(), "console service");
    TmCheckErr_(kMemPartitionInitGlobalScope(&tmMemoryPool, tmMemoryPoolBuf,
                                             128UL, 1UL),
                "memory pool");
    TM_INIT_SINGLETON_TASK(Worker, TmMemoryWorker, "TMA0", "A0D",
                           TM_STACKSIZE, TM_WORKER_PRIO);
    TM_INIT_SINGLETON_TASK(Report, TmMemoryReport, "TMR", "RPD",
                           TM_STACKSIZE, TM_REPORT_PRIO);
}

VOID TmMemoryWorker(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        VOID *const blockPtr = kMemPartitionAlloc(&tmMemoryPool);

        if (blockPtr == NULL)
        {
            printf("TM ERR %s memory allocation returned NULL\r\n",
                   TM_BENCH_NAME);
            tmErrors++;
            TmStopFault_();
        }
        TmCheckErr_(kMemPartitionFree(&tmMemoryPool, blockPtr),
                    "memory free");
        tmMemoryCounter++;
    }
}

VOID TmMemoryReport(VOID *args)
{
    ULONG lastCounter = 0UL;
    ULONG relativeMs = 0UL;

    RK_UNUSEARGS

    while (1)
    {
        kSleep(TmDurationTicks_());
        relativeMs += RK_THREAD_METRIC_TEST_DURATION_MS;

        ULONG const counter = tmMemoryCounter;
        if (counter == lastCounter)
        {
            printf("TM FAIL %s worker counter did not move\r\n",
                   TM_BENCH_NAME);
            tmErrors++;
        }

        TmReportSimple_(relativeMs, counter - lastCounter, counter);
        lastCounter = counter;
        TmAfterReport_();
    }
}
#endif
