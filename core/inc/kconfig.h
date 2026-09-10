/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/* KERNEL CONFIGURATION FILE                                                  */
/******************************************************************************/
#ifndef RK_CONFIG_H
#define RK_CONFIG_H

#define ON 1U
#define OFF 0U

/***[ MEMORY PROTECTION *******************************************************/
/* RK01 is a hardware MPU build. Non-MPU targets belong in a separate tree. */

/* FPU support is optional and build-selected. When enabled, the architecture
 * port must enable the coprocessor and preserve floating-point task context. */
#ifndef RK_CONF_FPU
#define RK_CONF_FPU (OFF)
#endif

#if ((RK_CONF_FPU != ON) && (RK_CONF_FPU != OFF))
#error "RK_CONF_FPU must be ON or OFF"
#endif

#if (RK_CONF_FPU == ON)
#ifndef __FPU_PRESENT
#error "RK_CONF_FPU=ON requires __FPU_PRESENT=1"
#elif (__FPU_PRESENT != 1)
#error "RK_CONF_FPU=ON requires __FPU_PRESENT=1"
#endif
#endif

/* RKFS is currently backed by reserved STM32F401RE flash. Other platforms can
 * keep the record example RAM-only by leaving this disabled. */
#ifndef RK_CONF_FILESYSTEM
#if defined(RK_MCU_F401RE)
#define RK_CONF_FILESYSTEM (ON)
#else
#define RK_CONF_FILESYSTEM (OFF)
#endif
#endif

#if ((RK_CONF_FILESYSTEM != ON) && (RK_CONF_FILESYSTEM != OFF))
#error "RK_CONF_FILESYSTEM must be ON or OFF"
#endif

#if ((RK_CONF_FILESYSTEM == ON) && !defined(RK_MCU_F401RE))
#error "RK_CONF_FILESYSTEM=ON currently requires RK_MCU_F401RE"
#endif

/* RK01 keeps one region for domain RAM, one for the private task stack, one
 * for the global shared aperture and leaves the remaining regions for
 * explicitly mapped inter-domain regions. */
#ifndef RK_CONF_DOMAIN_SHARED_REGIONS
#define RK_CONF_DOMAIN_SHARED_REGIONS (4U)
#endif

/* Default to containing unprivileged MPU task faults so the runtime can retire
 * only the offending task and release/poison any objects it owned. Define this
 * to ON in a board profile when first-fault debugger stop is preferred. */
#ifndef RK_CONF_MPU_TASK_FAULT_FAIL_FAST
#define RK_CONF_MPU_TASK_FAULT_FAIL_FAST (OFF)
#endif

/***[ INTERRUPT PRIORITIES ****************************************************/
#ifndef __NVIC_PRIO_BITS
#error "__NVIC_PRIO_BITS must be defined by the platform or build profile"
#elif (__NVIC_PRIO_BITS > 0)
#define RK_CONF_NPRIO_BITS __NVIC_PRIO_BITS
#else
#error "__NVIC_PRIO_BITS must be greater than zero"
#endif

/***[ CRITICAL SECTIONS *******************************************************/
/* PRIMASK preserves the original RK01 behaviour: RK_ENTER_CR() disables all
 * maskable interrupts. BASEPRI leaves higher-urgency interrupts unmasked and
 * masks only priorities at or below RK_CONF_CR_BASEPRI_PRIO. */
#define RK_CONF_CR_PRIMASK (0U)
#define RK_CONF_CR_BASEPRI (1U)

#ifndef RK_CONF_CR_MASK
#define RK_CONF_CR_MASK RK_CONF_CR_PRIMASK
#endif

#ifndef RK_CONF_CR_BASEPRI_PRIO
#define RK_CONF_CR_BASEPRI_PRIO (1U)
#endif

#if ((RK_CONF_CR_MASK != RK_CONF_CR_PRIMASK) && \
     (RK_CONF_CR_MASK != RK_CONF_CR_BASEPRI))
#error "RK_CONF_CR_MASK must be RK_CONF_CR_PRIMASK or RK_CONF_CR_BASEPRI"
#endif

/******************************************************************************/
/********* 1. TASKS AND SCHEDULER *********************************************/
/******************************************************************************/

/*** [  SYSTEM TASKS STACK SIZE (WORDS) **************************************/
/******************************************************************************/
/* This configuration is exposed so the system programmer can adjust          */
/* the IdleTask stack size to support any hook.                               */
/*                                                                            */
/* The Post-Processing system task stack size must be adjusted to support     */
/* Application Timers callouts.                                               */
/* (!) Minimal stack size is 128                                              */
/* (!) Keep it aligned to a double-word (8-byte) boundary.                    */
/******************************************************************************/
#ifndef RK_CONF_IDLE_STACKSIZE
#define RK_CONF_IDLE_STACKSIZE (128U) /* Words */
#endif

#ifndef RK_CONF_POSTPROC_STACKSIZE
#define RK_CONF_POSTPROC_STACKSIZE (256U) /* Words */
#endif

/***[ OBJECT NAME LENGTH ****************************************************/

#ifndef RK_CONF_MAX_NAME_LEN
#define RK_CONF_MAX_NAME_LEN (8U)
#endif

#ifndef RK_OBJ_MAX_NAME_LEN
#define RK_OBJ_MAX_NAME_LEN RK_CONF_MAX_NAME_LEN
#endif

#ifndef RK_CONF_MIN_PRIO
#define RK_CONF_MIN_PRIO 31
#endif

/***[ KERNEL CONSOLE UART SERVICE ********************************************/
/*
 * The console UART is owned by a privileged kernel driver task. The low-level
 * board path stays available for panic/early output, but normal RX ownership and
 * synchronous TX requests flow through this service.
 */
#ifndef RK_CONF_CONSOLE_SERVICE_STACKSIZE
#define RK_CONF_CONSOLE_SERVICE_STACKSIZE (128U)
#endif

#ifndef RK_CONF_CONSOLE_SERVICE_PRIO
#define RK_CONF_CONSOLE_SERVICE_PRIO RK_CONF_MIN_PRIO
#endif

#ifndef RK_CONF_CONSOLE_SERVICE_POLL_TICKS
#define RK_CONF_CONSOLE_SERVICE_POLL_TICKS (1U)
#endif

/* Maximum printable bytes in one terminal command line, excluding CR/LF/NUL. */
#ifndef RK_CONF_CONSOLE_LINE_MAX_BYTES
#define RK_CONF_CONSOLE_LINE_MAX_BYTES (64U)
#endif

#ifndef RK_CONF_CONSOLE_RX_PENDING_LINES
#define RK_CONF_CONSOLE_RX_PENDING_LINES (1U)
#endif

#ifndef RK_CONF_CONSOLE_RX_BUFFER_BYTES
#define RK_CONF_CONSOLE_RX_BUFFER_BYTES                                      \
    ((RK_CONF_CONSOLE_LINE_MAX_BYTES + 1U) * RK_CONF_CONSOLE_RX_PENDING_LINES)
#endif

#ifndef RK_CONF_CONSOLE_WRITE_MAX_BYTES
#define RK_CONF_CONSOLE_WRITE_MAX_BYTES                                  \
    (RK_CONF_CONSOLE_LINE_MAX_BYTES + 1UL)
#endif

#if (RK_CONF_CONSOLE_SERVICE_STACKSIZE < 128U)
#error "RK_CONF_CONSOLE_SERVICE_STACKSIZE must be at least 128 words"
#endif
#if (RK_CONF_CONSOLE_SERVICE_PRIO > RK_CONF_MIN_PRIO)
#error "RK_CONF_CONSOLE_SERVICE_PRIO must be <= RK_CONF_MIN_PRIO"
#endif
#if (RK_CONF_CONSOLE_SERVICE_POLL_TICKS == 0U)
#error "RK_CONF_CONSOLE_SERVICE_POLL_TICKS must be non-zero"
#endif
#if (RK_CONF_CONSOLE_LINE_MAX_BYTES < 8U)
#error "RK_CONF_CONSOLE_LINE_MAX_BYTES must be at least 8"
#endif
#if (RK_CONF_CONSOLE_RX_PENDING_LINES == 0U)
#error "RK_CONF_CONSOLE_RX_PENDING_LINES must be non-zero"
#endif
#if (RK_CONF_CONSOLE_RX_BUFFER_BYTES < (RK_CONF_CONSOLE_LINE_MAX_BYTES + 1U))
#error "RK_CONF_CONSOLE_RX_BUFFER_BYTES must hold at least one full line"
#endif
#if (RK_CONF_CONSOLE_WRITE_MAX_BYTES == 0UL)
#error "RK_CONF_CONSOLE_WRITE_MAX_BYTES must be non-zero"
#endif

/***[ SYSTEM MONITOR TERMINAL ************************************************/
#ifndef RK_CONF_SYSMON
/* SysMon is opt-in diagnostics. APP_EXAMPLE=04-sysmon enables it explicitly. */
#define RK_CONF_SYSMON (OFF)
#endif

#if ((RK_CONF_SYSMON != ON) && (RK_CONF_SYSMON != OFF))
#error "RK_CONF_SYSMON must be ON or OFF"
#endif

#if (RK_CONF_SYSMON == ON)
#ifndef RK_CONF_SYSMON_STACKSIZE
#define RK_CONF_SYSMON_STACKSIZE (512U)
#endif
#ifndef RK_CONF_SYSMON_PRIO
#define RK_CONF_SYSMON_PRIO (30U)
#endif
#ifndef RK_CONF_SYSMON_LINE_LEN
#define RK_CONF_SYSMON_LINE_LEN (RK_CONF_CONSOLE_LINE_MAX_BYTES + 1U)
#endif
#ifndef RK_CONF_SYSMON_POLL_TICKS
#define RK_CONF_SYSMON_POLL_TICKS (50U)
#endif
#ifndef RK_CONF_SYSMON_SNAPSHOT_MAX
#if defined(STM32F401xE)
#define RK_CONF_SYSMON_SNAPSHOT_MAX (8U)
#else
#define RK_CONF_SYSMON_SNAPSHOT_MAX (16U)
#endif
#endif
#if (RK_CONF_SYSMON_LINE_LEN < 8U)
#error "RK_CONF_SYSMON_LINE_LEN must be at least 8"
#endif
#if (RK_CONF_SYSMON_POLL_TICKS == 0U)
#error "RK_CONF_SYSMON_POLL_TICKS must be non-zero"
#endif
#if (RK_CONF_SYSMON_SNAPSHOT_MAX == 0U)
#error "RK_CONF_SYSMON_SNAPSHOT_MAX must be non-zero"
#endif
#endif

/***[ DYNAMIC TASK CREATION **************************************************/
/* Enables/disables runtime task creation via kTaskSpawn(). */
#ifndef RK_CONF_DYNAMIC_TASK
#define RK_CONF_DYNAMIC_TASK (ON)
#endif

/***[ DYNAMIC KERNEL OBJECT POOLS ********************************************/
/* Runtime object creation is controlled by each fixed pool capacity below.
 * Set a pool maximum to zero to compile out that object family's pool. */
#ifndef RK_CONF_DYNAMIC_SEMAPHORES_MAX
#define RK_CONF_DYNAMIC_SEMAPHORES_MAX (4U)
#endif

#ifndef RK_CONF_DYNAMIC_MUTEXES_MAX
#define RK_CONF_DYNAMIC_MUTEXES_MAX (4U)
#endif

#ifndef RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX
#define RK_CONF_DYNAMIC_SLEEP_QUEUES_MAX (4U)
#endif

#ifndef RK_CONF_DYNAMIC_MESG_QUEUES_MAX
#define RK_CONF_DYNAMIC_MESG_QUEUES_MAX (2U)
#endif

#ifndef RK_CONF_DYNAMIC_TIMERS_MAX
#define RK_CONF_DYNAMIC_TIMERS_MAX (2U)
#endif

#ifndef RK_CONF_DYNAMIC_MRMS_MAX
#define RK_CONF_DYNAMIC_MRMS_MAX (2U)
#endif

#ifndef RK_CONF_DYNAMIC_SHARED_MEMS_MAX
#define RK_CONF_DYNAMIC_SHARED_MEMS_MAX (2U)
#endif

/***[ MAXIMUM NUMBER OF USER TASKS  ******************************************/
/*
Maximum number of user tasks supported by the kernel, including tasks to be
created after the scheduler starts (so-called "dynamic tasks")
If using the Application Logger facility, the Logger Task should be taken into
account.
 */
#ifndef RK_CONF_N_USRTASKS_MAX
#define RK_CONF_N_USRTASKS_MAX (31)
#endif

/***[ SYSTEM CORE CLOCK  *****************************************************/
/* If this is set to 0, RK01 uses the platform default clock below. If a board  */
/* port has no default clock, the ARM core port falls back to CMSIS            */
/* SystemCoreClock. CMSIS-Core is not bundled in RK0.                         */
#ifndef RK_CONF_SYSCORECLK

#define RK_CONF_SYSCORECLK (0UL)

#endif

#ifndef RK_CONF_PLATFORM_SYSCORECLK
#if defined(STM32F401xE)
#define RK_CONF_PLATFORM_SYSCORECLK (80000000UL)
#elif defined(RK_MCU_MPS2_AN386)
#define RK_CONF_PLATFORM_SYSCORECLK (25000000UL)
#elif defined(RK_MCU_MPS2_AN505)
#define RK_CONF_PLATFORM_SYSCORECLK (20000000UL)
#else
#define RK_CONF_PLATFORM_SYSCORECLK (0UL)
#endif
#endif

#ifndef RK_CONF_EFFECTIVE_SYSCORECLK
#if (RK_CONF_SYSCORECLK > 0UL)
#define RK_CONF_EFFECTIVE_SYSCORECLK (RK_CONF_SYSCORECLK)
#else
#define RK_CONF_EFFECTIVE_SYSCORECLK (RK_CONF_PLATFORM_SYSCORECLK)
#endif
#endif

/***[ KERNEL TICK *************************************************************/
/* This will set the tick as 1/RK_SYSTICK_DIV millisec                        */
/* 1000 -> 1 ms Tick, 500 -> 2 ms Tick, 100 -> 10ms Tick, and so forth        */
/* Recommended tick for applications running on low-end devices is 10ms       */
#ifndef RK_CONF_SYSTICK_DIV
#define RK_CONF_SYSTICK_DIV (100UL)
#endif
/***[ MILLISEC TO TICK GRANULARITY ********************************************/
/* This setting defines if asking to convert a time value in milliseconds that
 * is less than 1 TICK it rounds up to 1 or returns 0
 */
#ifndef RK_CONF_ROUND_UP_MS_TO_TICKS
#define RK_CONF_ROUND_UP_MS_TO_TICKS  (OFF)
#endif


/******************************************************************************/
/********* 2. APPLICATION TIMER  **********************************************/
/******************************************************************************/

#ifndef RK_CONF_CALLOUT_TIMER
#define RK_CONF_CALLOUT_TIMER (ON)
#endif

/******************************************************************************/
/********* 3. INTER-TASK COMMUNICATION ****************************************/
/******************************************************************************/

/*** SHARED-STATE MECHANISMS ***/

/* SEMAPHORES (COUNTING/BINARY) */
#ifndef RK_CONF_SEMAPHORE
#define RK_CONF_SEMAPHORE (ON)
#endif

/* MUTEX LOCK */
#ifndef RK_CONF_MUTEX
#define RK_CONF_MUTEX (ON)
#endif

/* SLEEP QUEUE */
#ifndef RK_CONF_SLEEP_QUEUE
#define RK_CONF_SLEEP_QUEUE (ON)
#endif

#if (RK_CONF_SLEEP_QUEUE == ON && RK_CONF_MUTEX == ON)
/* Condition Variable Model Helpers */
#ifndef RK_CONF_CONDVAR
#define RK_CONF_CONDVAR (ON)
#endif
#endif


/*** MESSAGE-PASSING MECHANISMS ***/

/* MESSAGE QUEUE  */

#ifndef RK_CONF_MESG_QUEUE
#define RK_CONF_MESG_QUEUE (ON)
#endif

#if (RK_CONF_MESG_QUEUE == ON)

#ifndef RK_CONF_MESG_QUEUE_SEND_CALLBACK
#define RK_CONF_MESG_QUEUE_SEND_CALLBACK (OFF)
#endif

/*
 * Asynchronous task-addressed messages.
 *
 * RK_CONF_ASYNCH_MESG is the legacy umbrella switch for this subsystem. When
 * enabled, it exposes the by-reference direct RK_MESG APIs:
 * kMesgAlloc(), kMesgSend(), kMesgWait() and kMesgFree(). Those pointers
 * remain caller-owned memory, so both endpoints must be able to access the
 * message pool memory under MPU.
 *
 * RK_CONF_ASYNCH_COPY_MESG below enables the task-addressed copy-message path
 * that uses a bounded kernel-owned RK_MESG pool for cross-domain payloads.
 */
#ifndef RK_CONF_ASYNCH_MESG
#define RK_CONF_ASYNCH_MESG (ON)
#endif

#if (RK_CONF_ASYNCH_MESG == ON)
/*
 * Task-addressed asynchronous copy messages. These buffers are owned by the
 * kernel and are never returned to user code as RK_MESG pointers.
 */
#ifndef RK_CONF_ASYNCH_COPY_MESG
#define RK_CONF_ASYNCH_COPY_MESG (ON)
#endif

#if (RK_CONF_ASYNCH_COPY_MESG == ON)
#ifndef RK_CONF_ASYNCH_COPY_MESG_MAX
#define RK_CONF_ASYNCH_COPY_MESG_MAX (8U)
#endif

#ifndef RK_CONF_ASYNCH_COPY_MESG_PAYLOAD_BYTES
#define RK_CONF_ASYNCH_COPY_MESG_PAYLOAD_BYTES (32UL)
#endif

#if (RK_CONF_ASYNCH_COPY_MESG_MAX == 0U)
#error "RK_CONF_ASYNCH_COPY_MESG_MAX must be greater than zero"
#endif

#if (RK_CONF_ASYNCH_COPY_MESG_PAYLOAD_BYTES == 0UL)
#error "RK_CONF_ASYNCH_COPY_MESG_PAYLOAD_BYTES must be greater than zero"
#endif
#endif
#endif

#endif /* RK_CONF_MESG_QUEUE */

/* SYNCHRONOUS UNBUFFERED MESSAGE */
#ifndef RK_CONF_SYNCH_MESG
#define RK_CONF_SYNCH_MESG (ON)
#endif

/* MRM PROTOCOL */
#ifndef RK_CONF_MRM
#define RK_CONF_MRM (OFF)
#endif

#if (RK_CONF_MRM == ON)
#ifndef RK_CONF_MRM_LEASES_MAX
#define RK_CONF_MRM_LEASES_MAX (4U)
#endif
#if (RK_CONF_MRM_LEASES_MAX == 0U)
#error "RK_CONF_MRM_LEASES_MAX must be greater than zero"
#endif
#endif

/******************************************************************************/
/********* 4. ERROR CHECKING    ***********************************************/
/******************************************************************************/
/* The kernel can return error codes (RK_CONF_ERR_CHECK) plus also halting    */
/* execution (RK_CONF_FAULT) upon faulty operations request, such as a        */
/* blocking call within an ISR.                                               */
/* Note that an unsuccessful return value is not synonymous with error.       */
/* An unsuccesful 'try' post to a full single-slot queue or a 'signal' to a   */
/* empty RK_SLEEP_QUEUE, for instance are well-defined operations, that do not*/
/* lead to system failure.                                                    */
/* SUCCESSFUL operations return 0. UNSUCCESFUL are > 0. ERRORS are < 0.       */

#if !defined(NDEBUG)
#ifndef RK_CONF_ERR_CHECK
#define RK_CONF_ERR_CHECK (ON)
#endif
#if (RK_CONF_ERR_CHECK == ON)
#define RK_CONF_FAULT (ON)
#define RK_CONF_FAULT_PRINT_STDERR (ON)
#endif
#endif

#ifndef RK_CONF_FAULT
#define RK_CONF_FAULT (OFF)
#endif

#ifndef RK_CONF_FAULT_PRINT_STDERR
#define RK_CONF_FAULT_PRINT_STDERR (OFF)
#endif

/* Print contained MemManage task faults as they happen when fatal printing is
 * enabled. This leaves task-fault containment on while preserving first-cause
 * diagnostics on the board console. */
#ifndef RK_CONF_MPU_TASK_FAULT_PRINT_STDERR
#define RK_CONF_MPU_TASK_FAULT_PRINT_STDERR RK_CONF_FAULT_PRINT_STDERR
#endif

#if ((RK_CONF_MPU_TASK_FAULT_PRINT_STDERR != ON) && \
     (RK_CONF_MPU_TASK_FAULT_PRINT_STDERR != OFF))
#error "RK_CONF_MPU_TASK_FAULT_PRINT_STDERR must be ON or OFF"
#endif

#endif /* KCONFIG_H */
