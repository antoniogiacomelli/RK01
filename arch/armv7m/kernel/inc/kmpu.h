/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_MPU_H
#define RK_MPU_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <kcommondefs.h>
#include <khal.h>

/* Cortex-M4 implements 8 programmable MPU data regions on STM32F401RE. */
#if defined(RK_MCU_F401RE)
#define RK_MPU_N_REGIONS (8U)
#endif

/* RK01-reserved region slots. Region 0 is static. The domain-map slots are
 * loaded only when dispatch crosses to a different immutable domain map; the
 * private stack slot follows the incoming task. */
#define RK_MPU_REGION_USER_FLASH (0U)
#define RK_MPU_REGION_DOMAIN_RAM (1U)
#define RK_MPU_REGION_TASK_STACK (2U)
#define RK_MPU_REGION_SHARED_RAM (3U)
#define RK_MPU_REGION_EXPLICIT_SHARED_BASE (4U)

#if (RK_CONF_DOMAIN_SHARED_REGIONS >                                      \
     (RK_MPU_N_REGIONS - RK_MPU_REGION_EXPLICIT_SHARED_BASE))
#error "RK_CONF_DOMAIN_SHARED_REGIONS exceeds available MPU regions"
#endif

/* CONTROL values saved in each TCB. RK01 tasks run on PSP; bit 0 selects
 * privileged/unprivileged thread mode. */
#define RK_CONTROL_PSP_PRIVILEGED (0x2UL)
#define RK_CONTROL_PSP_UNPRIVILEGED (0x3UL)
#define RK_CONTROL_FPCA (0x4UL)

/* MPU TYPE.DREGION reports the number of implemented data regions. */
#define RK_MPU_TYPE_DREGION(value) (((value) >> 8U) & 0xFFUL)

/* MPU CTRL bits. RK01 enables PRIVDEFENA so privileged code can still use the
 * default system memory map while user tasks are restricted by regions. */
#define RK_MPU_CTRL_ENABLE (1UL << 0U)
#define RK_MPU_CTRL_HFNMIENA (1UL << 1U)
#define RK_MPU_CTRL_PRIVDEFENA (1UL << 2U)

/* System Handler Control and State Register bit enabling MemManage faults. */
#define RK_SCB_SHCSR_MEMFAULTENA (1UL << 16U)

/* RBAR.VALID lets a single RBAR write also select the target region number. */
#define RK_MPU_RBAR_VALID (1UL << 4U)
#define RK_MPU_RBAR_REGION_MASK (0xFUL)

/* RASR attribute helpers. Region size is encoded separately by kMpuBuildRegion()
 * because it depends on the power-of-two byte length. */
#define RK_MPU_RASR_ENABLE (1UL << 0U)
#define RK_MPU_RASR_XN (1UL << 28U)

#define RK_MPU_RASR_AP(value) (((ULONG)(value) & 0x7UL) << 24U)
#define RK_MPU_RASR_TEX(value) (((ULONG)(value) & 0x7UL) << 19U)
#define RK_MPU_RASR_S (1UL << 18U)
#define RK_MPU_RASR_C (1UL << 17U)
#define RK_MPU_RASR_B (1UL << 16U)
#define RK_MPU_RASR_SRD(value) (((ULONG)(value) & 0xFFUL) << 8U)

#define RK_MPU_AP_NONE_NONE (0U)
#define RK_MPU_AP_RW_NONE (1U)
#define RK_MPU_AP_RW_RO (2U)
#define RK_MPU_AP_RW_RW (3U)
#define RK_MPU_AP_RO_NONE (5U)
#define RK_MPU_AP_RO_RO (6U)

/* User FLASH is executable and read-only to both privileged and unprivileged
 * code. */
#define RK_MPU_ATTR_USER_FLASH                                               \
    (RK_MPU_RASR_AP(RK_MPU_AP_RO_RO) | RK_MPU_RASR_C)

/* User SRAM is read/write, shareable/cacheable, and execute-never. */
#define RK_MPU_ATTR_USER_SRAM                                                \
    (RK_MPU_RASR_XN | RK_MPU_RASR_AP(RK_MPU_AP_RW_RW) | RK_MPU_RASR_S |     \
     RK_MPU_RASR_C)

/* Peripheral windows should be data-only, shareable, and bufferable. */
#define RK_MPU_ATTR_USER_PERIPHERAL                                          \
    (RK_MPU_RASR_XN | RK_MPU_RASR_AP(RK_MPU_AP_RW_RW) | RK_MPU_RASR_S |     \
     RK_MPU_RASR_B)

/* Explicit deny region, useful for future guard holes or subregion schemes. */
#define RK_MPU_ATTR_NO_ACCESS                                                \
    (RK_MPU_RASR_XN | RK_MPU_RASR_AP(RK_MPU_AP_NONE_NONE))

/* Precomputed hardware region values. TCBs store these so context switch only
 * writes RBAR/RASR and does not recalculate MPU encoding. */
typedef struct RK_STRUCT_MPU_REGION
{
    ULONG rbar;
    ULONG rasr;
} RK_MPU_REGION;

/* Core integer portion of a Cortex-M hardware-stacked exception frame. When an
 * extended FP frame is present, the assembly handlers adjust the raw stack
 * pointer before passing this frame to C code. */
typedef struct RK_STRUCT_EXCEPTION_FRAME
{
    ULONG r0;
    ULONG r1;
    ULONG r2;
    ULONG r3;
    ULONG r12;
    ULONG lr;
    ULONG pc;
    ULONG xpsr;
} RK_EXCEPTION_FRAME;

/* Last captured MPU fault. Kept global so a debugger can inspect why an
 * unprivileged task was terminated or why the kernel stopped. */
typedef struct RK_STRUCT_MPU_FAULT_INFO
{
    RK_TID tid;
    ULONG pc;
    ULONG lr;
    ULONG xpsr;
    ULONG cfsr;
    ULONG mmfsr;
    ULONG faultAddress;
    ULONG excReturn;
    ULONG psp;
    ULONG control;
    ULONG frameValid;
} RK_MPU_FAULT_INFO;

extern volatile RK_MPU_FAULT_INFO RK_gMpuFaultInfo;

/* Initialise the MPU global state and permanent user FLASH region. */
VOID kMpuInit(VOID);

/* Load the current RK_gRunPtr task's MPU view. Called from task startup and
 * context-switch assembly. */
VOID kMpuLoadRunTask(VOID);

/* Save/restore CONTROL across context switches so each task resumes with its
 * own PSP privilege state. */
VOID kMpuSaveRunTaskControl(ULONG const control);
ULONG kMpuGetRunTaskControl(VOID);

/* Build an MPU region descriptor from validated base/size/attribute inputs. */
RK_ERR kMpuBuildRegion(RK_MPU_REGION *const regionPtr,
                       UINT const regionNumber,
                       ULONG const baseAddress,
                       ULONG const regionSize,
                       ULONG const attributes);

/* Validate and reserve the task's domain RAM plus private-stack mapping.
 * A NULL domain in RK_TASK_MEMORY creates the private domain used by explicit
 * isolated tasks. */
RK_BOOL kMpuTaskMemoryValid(RK_TASK_MEMORY const *const memoryPtr);
RK_ERR kMpuTaskMemoryReserve(RK_TCB *const taskPtr,
                             RK_TASK_MEMORY const *const memoryPtr);

/* Freeze the boot-created MPU memory layout before the first task dispatch. */
RK_BOOL kMpuLayoutIsFinalized(VOID);
RK_ERR kMpuLayoutFinalize(VOID);

/* Add standard shared regions to a task's TCB region table. */
RK_ERR kMpuTaskAttachSharedRegions(RK_TCB *const taskPtr);

/* Validate user-mode memory access against the task's MPU contract. These
 * helpers are used by SVC before privileged code dereferences user pointers. */
RK_BOOL kMpuUserReadValid(RK_TCB const *const taskPtr,
                          VOID const *const ptr,
                          ULONG const bytes);
RK_BOOL kMpuUserWriteValid(RK_TCB const *const taskPtr,
                           VOID *const ptr,
                           ULONG const bytes);
RK_BOOL kMpuUserFunctionValid(VOID const *const funPtr);

/* Validate domain RAM: the statically declared writable-authority region used
 * by member tasks. Member stacks are private task regions outside this window. */
RK_BOOL kMpuDomainMemoryValid(RK_DOMAIN const *const domainPtr);
RK_ERR kMpuDomainMemoryReserve(RK_DOMAIN *const domainPtr);

/* Validate and reserve an explicit shared memory region that selected domains
 * can map in addition to the global shared RAM aperture. */
RK_BOOL kMpuSharedRegionMemoryValid(RK_SHARED_REGION const *const regionPtr);
RK_ERR kMpuSharedRegionMemoryReserve(RK_SHARED_REGION *const regionPtr);
VOID kMpuSharedRegionMemoryRelease(RK_SHARED_REGION *const regionPtr);

/* Release a task's private one-task domain reservation during TCB teardown. */
VOID kMpuTaskMemoryRelease(RK_TCB *const taskPtr);

/* C half of MemManage_Handler. Captures fault info and either terminates the
 * offending unprivileged task or treats privileged faults as fatal. */
VOID kMemManageFault(RK_EXCEPTION_FRAME *const framePtr,
                     ULONG const excReturn);

/* C half of SVC_Handler. Declared here because SVC and MPU exception plumbing
 * share the same exception-frame type. */
VOID kSyscallDispatch(RK_EXCEPTION_FRAME *const framePtr,
                      ULONG const excReturn,
                      ULONG const svcNumber);

#ifdef __cplusplus
}
#endif

#endif /* RK_MPU_H */
