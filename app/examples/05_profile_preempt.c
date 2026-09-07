/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Profiling example 05: ThreadX-style preemptive scheduling counter test.
 * Build one dispatch class per binary.
 *
 * Conditions matching the RK0 profiling page:
 *   make APP_EXAMPLE=05-profile-preempt FPU=OFF \
 *        EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000"
 *
 * Per-task-domain mode:
 *   make APP_EXAMPLE=05-profile-preempt FPU=OFF \
 *        EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000 -DPROFILE_PREEMPT_CLASS=PROFILE_PREEMPT_CLASS_PER_TASK_DOMAIN"
 */

#include <kapi_domain.h>
#include <kconsole.h>

#define TM_TEST_DURATION_MS (30000UL)
#define TM_FLAG RK_EVENT_1
#define STACKSIZE (128U)
#define PROFILE_DOMAIN_BYTES (32U)
#define PROFILE_PREEMPT_CLASS_SAME_DOMAIN (1U)
#define PROFILE_PREEMPT_CLASS_PER_TASK_DOMAIN (2U)

#ifndef PROFILE_PREEMPT_CLASS
#define PROFILE_PREEMPT_CLASS PROFILE_PREEMPT_CLASS_SAME_DOMAIN
#endif

#if ((PROFILE_PREEMPT_CLASS != PROFILE_PREEMPT_CLASS_SAME_DOMAIN) &&         \
     (PROFILE_PREEMPT_CLASS != PROFILE_PREEMPT_CLASS_PER_TASK_DOMAIN))
#error "PROFILE_PREEMPT_CLASS must be PROFILE_PREEMPT_CLASS_SAME_DOMAIN or PROFILE_PREEMPT_CLASS_PER_TASK_DOMAIN"
#endif

#if (PROFILE_PREEMPT_CLASS == PROFILE_PREEMPT_CLASS_PER_TASK_DOMAIN)
#define PROFILE_CLASS_NAME "per-task-domain"
#define PROFILE_STATE_ATTR K_ALIGN(4) RK_SECTION_SHARED_BSS
RK_DECLARE_DOMAIN(task1Domain, task1Ram, PROFILE_DOMAIN_BYTES)
RK_DECLARE_DOMAIN(task2Domain, task2Ram, PROFILE_DOMAIN_BYTES)
RK_DECLARE_DOMAIN(task3Domain, task3Ram, PROFILE_DOMAIN_BYTES)
RK_DECLARE_DOMAIN(task4Domain, task4Ram, PROFILE_DOMAIN_BYTES)
RK_DECLARE_DOMAIN(task5Domain, task5Ram, PROFILE_DOMAIN_BYTES)
RK_DECLARE_DOMAIN(task6Domain, task6Ram, PROFILE_DOMAIN_BYTES)
RK_DECLARE_DOMAIN_TASK(task1Handle, Task1)
RK_DECLARE_DOMAIN_TASK(task2Handle, Task2)
RK_DECLARE_DOMAIN_TASK(task3Handle, Task3)
RK_DECLARE_DOMAIN_TASK(task4Handle, Task4)
RK_DECLARE_DOMAIN_TASK(task5Handle, Task5)
RK_DECLARE_DOMAIN_TASK(task6Handle, Task6)
RK_DECLARE_DOMAIN_TASK_STACK(stack1, STACKSIZE)
RK_DECLARE_DOMAIN_TASK_STACK(stack2, STACKSIZE)
RK_DECLARE_DOMAIN_TASK_STACK(stack3, STACKSIZE)
RK_DECLARE_DOMAIN_TASK_STACK(stack4, STACKSIZE)
RK_DECLARE_DOMAIN_TASK_STACK(stack5, STACKSIZE)
RK_DECLARE_DOMAIN_TASK_STACK(stack6, STACKSIZE)
#else
#define PROFILE_CLASS_NAME "same-domain"
#define PROFILE_STATE_ATTR
RK_DECLARE_TASK(task1Handle, Task1, stack1, STACKSIZE)
RK_DECLARE_TASK(task2Handle, Task2, stack2, STACKSIZE)
RK_DECLARE_TASK(task3Handle, Task3, stack3, STACKSIZE)
RK_DECLARE_TASK(task4Handle, Task4, stack4, STACKSIZE)
RK_DECLARE_TASK(task5Handle, Task5, stack5, STACKSIZE)
RK_DECLARE_TASK(task6Handle, Task6, stack6, STACKSIZE)
#endif

static volatile ULONG counter1 PROFILE_STATE_ATTR;
static volatile ULONG counter2 PROFILE_STATE_ATTR;
static volatile ULONG counter3 PROFILE_STATE_ATTR;
static volatile ULONG counter4 PROFILE_STATE_ATTR;
static volatile ULONG counter5 PROFILE_STATE_ATTR;
static volatile ULONG error PROFILE_STATE_ATTR;
static volatile ULONG roundn PROFILE_STATE_ATTR;

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

static ULONG ProfileAbsDelta_(ULONG const value, ULONG const average)
{
    return ((value >= average) ? (value - average) : (average - value));
}

static VOID ProfilePrintField_(CHAR const *const namePtr, ULONG const value)
{
    kPuts(namePtr);
    ProfilePrintUL_(value);
}

static VOID ProfileReport_(RK_TICK const time0, RK_TICK const time1)
{
    ULONG const c1 = counter1;
    ULONG const c2 = counter2;
    ULONG const c3 = counter3;
    ULONG const c4 = counter4;
    ULONG const c5 = counter5;
    ULONG const total = c1 + c2 + c3 + c4 + c5;
    ULONG const average = total / 5UL;
    ULONG maxDelta = ProfileAbsDelta_(c1, average);
    ULONG delta = ProfileAbsDelta_(c2, average);

    if (delta > maxDelta)
    {
        maxDelta = delta;
    }
    delta = ProfileAbsDelta_(c3, average);
    if (delta > maxDelta)
    {
        maxDelta = delta;
    }
    delta = ProfileAbsDelta_(c4, average);
    if (delta > maxDelta)
    {
        maxDelta = delta;
    }
    delta = ProfileAbsDelta_(c5, average);
    if (delta > maxDelta)
    {
        maxDelta = delta;
    }

    if ((c1 + 1UL < average) || (c1 > average + 1UL) ||
        (c2 + 1UL < average) || (c2 > average + 1UL) ||
        (c3 + 1UL < average) || (c3 > average + 1UL) ||
        (c4 + 1UL < average) || (c4 > average + 1UL) ||
        (c5 + 1UL < average) || (c5 > average + 1UL))
    {
        error++;
    }

    kPuts("\r\nRK01 PROFILE PREEMPT class=");
    kPuts(PROFILE_CLASS_NAME);
    kPuts(" ");
    ProfilePrintField_("round=", roundn);
    ProfilePrintField_(" elapsed_ms=", time1 - time0);
    ProfilePrintField_(" tick_ms=", RK_TICK_INTERVAL_MS);
    ProfilePrintField_(" c1=", c1);
    ProfilePrintField_(" c2=", c2);
    ProfilePrintField_(" c3=", c3);
    ProfilePrintField_(" c4=", c4);
    ProfilePrintField_(" c5=", c5);
    ProfilePrintField_(" total=", total);
    ProfilePrintField_(" average=", average);
    ProfilePrintField_(" max_delta=", maxDelta);
    ProfilePrintField_(" errors=", error);
    kPuts("\r\n");
}

#define kSuspendSelf(timeout)                                                 \
    do                                                                        \
    {                                                                         \
        AppCheck_(kEventGet(TM_FLAG, RK_OPT_EVENT_ANY, NULL, (timeout)));     \
    } while (0)

#define kResumeTask(taskHandle)                                               \
    do                                                                        \
    {                                                                         \
        AppCheck_(kEventSet((taskHandle), TM_FLAG));                          \
    } while (0)

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
    AppCheck_(kConsoleServiceInit());

#if (PROFILE_PREEMPT_CLASS == PROFILE_PREEMPT_CLASS_PER_TASK_DOMAIN)
    AppCheck_(kDomainInit(&task1Domain, task1Ram, sizeof(task1Ram), "T0D"));
    AppCheck_(kDomainInit(&task2Domain, task2Ram, sizeof(task2Ram), "T1D"));
    AppCheck_(kDomainInit(&task3Domain, task3Ram, sizeof(task3Ram), "T2D"));
    AppCheck_(kDomainInit(&task4Domain, task4Ram, sizeof(task4Ram), "T3D"));
    AppCheck_(kDomainInit(&task5Domain, task5Ram, sizeof(task5Ram), "T4D"));
    AppCheck_(kDomainInit(&task6Domain, task6Ram, sizeof(task6Ram), "REPD"));

    AppCheck_(kTaskInitDomain(&task1Handle, Task1, RK_NO_ARGS, "T0", stack1,
                              STACKSIZE, 6U, RK_PREEMPT, &task1Domain));
    AppCheck_(kTaskInitDomain(&task2Handle, Task2, RK_NO_ARGS, "T1", stack2,
                              STACKSIZE, 5U, RK_PREEMPT, &task2Domain));
    AppCheck_(kTaskInitDomain(&task3Handle, Task3, RK_NO_ARGS, "T2", stack3,
                              STACKSIZE, 4U, RK_PREEMPT, &task3Domain));
    AppCheck_(kTaskInitDomain(&task4Handle, Task4, RK_NO_ARGS, "T3", stack4,
                              STACKSIZE, 3U, RK_PREEMPT, &task4Domain));
    AppCheck_(kTaskInitDomain(&task5Handle, Task5, RK_NO_ARGS, "T4", stack5,
                              STACKSIZE, 2U, RK_PREEMPT, &task5Domain));
    AppCheck_(kTaskInitDomain(&task6Handle, Task6, RK_NO_ARGS, "REP", stack6,
                              STACKSIZE, 1U, RK_PREEMPT, &task6Domain));
#else
    AppCheck_(kTaskInit(&task1Handle, Task1, RK_NO_ARGS, "T0", stack1,
                        STACKSIZE, 6U, RK_PREEMPT));
    AppCheck_(kTaskInit(&task2Handle, Task2, RK_NO_ARGS, "T1", stack2,
                        STACKSIZE, 5U, RK_PREEMPT));
    AppCheck_(kTaskInit(&task3Handle, Task3, RK_NO_ARGS, "T2", stack3,
                        STACKSIZE, 4U, RK_PREEMPT));
    AppCheck_(kTaskInit(&task4Handle, Task4, RK_NO_ARGS, "T3", stack4,
                        STACKSIZE, 3U, RK_PREEMPT));
    AppCheck_(kTaskInit(&task5Handle, Task5, RK_NO_ARGS, "T4", stack5,
                        STACKSIZE, 2U, RK_PREEMPT));
    AppCheck_(kTaskInit(&task6Handle, Task6, RK_NO_ARGS, "REP", stack6,
                        STACKSIZE, 1U, RK_PREEMPT));
#endif
}

VOID Task1(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        kResumeTask(task2Handle);
        counter1++;
    }
}

VOID Task2(VOID *args)
{
    RK_UNUSEARGS

    kSuspendSelf(RK_WAIT_FOREVER);

    while (1)
    {
        kResumeTask(task3Handle);
        counter2++;
        kSuspendSelf(RK_WAIT_FOREVER);
    }
}

VOID Task3(VOID *args)
{
    RK_UNUSEARGS

    kSuspendSelf(RK_WAIT_FOREVER);

    while (1)
    {
        kResumeTask(task4Handle);
        counter3++;
        kSuspendSelf(RK_WAIT_FOREVER);
    }
}

VOID Task4(VOID *args)
{
    RK_UNUSEARGS

    kSuspendSelf(RK_WAIT_FOREVER);

    while (1)
    {
        kResumeTask(task5Handle);
        counter4++;
        kSuspendSelf(RK_WAIT_FOREVER);
    }
}

VOID Task5(VOID *args)
{
    RK_UNUSEARGS

    kSuspendSelf(RK_WAIT_FOREVER);

    while (1)
    {
        counter5++;
        kSuspendSelf(RK_WAIT_FOREVER);
    }
}

VOID Task6(VOID *args)
{
    RK_UNUSEARGS

    while (1)
    {
        RK_TICK const time0 = kTickGetMs();

        AppCheck_(kSleep(RK_MS_TO_TICKS(TM_TEST_DURATION_MS)));
        roundn++;
        ProfileReport_(time0, kTickGetMs());
    }
}
