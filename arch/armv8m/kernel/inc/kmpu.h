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

/* RK01 requires at least these eight task-programmable MPU regions on M33. */
#if defined(RK_MCU_MPS2_AN505)
#define RK_MPU_N_REGIONS (8U)
#endif

/* RK01-reserved region slots. Region 0 is static. The private stack slot is
 * replaced on every different-task dispatch; domain/shared slots are replaced
 * only on a domain boundary. */
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

/* ARMv8-M MPU region registers. Base and limit are 32-byte granularity. */
#define RK_MPU_GRANULE_BYTES (32UL)
#define RK_MPU_GRANULE_MASK (RK_MPU_GRANULE_BYTES - 1UL)
#define RK_MPU_RBAR_BASE_MASK (0xFFFFFFE0UL)
#define RK_MPU_RBAR_XN (1UL << 0U)
#define RK_MPU_RBAR_AP(value) (((ULONG)(value) & 0x3UL) << 1U)
#define RK_MPU_RBAR_SH(value) (((ULONG)(value) & 0x3UL) << 3U)
#define RK_MPU_RLAR_ENABLE (1UL << 0U)
#define RK_MPU_RLAR_ATTR_INDEX(value) (((ULONG)(value) & 0x7UL) << 1U)
#define RK_MPU_RLAR_LIMIT_MASK (0xFFFFFFE0UL)

#define RK_MPU_SH_NON_SHAREABLE (0U)
#define RK_MPU_SH_OUTER_SHAREABLE (2U)
#define RK_MPU_SH_INNER_SHAREABLE (3U)

#define RK_MPU_AP_PRIV_RW (0U)
#define RK_MPU_AP_FULL_ACCESS (1U)
#define RK_MPU_AP_PRIV_RO (2U)
#define RK_MPU_AP_READ_ONLY (3U)

#define RK_MPU_MAIR_NORMAL_NC (0x44UL)
#define RK_MPU_MAIR_DEVICE_NGNRE (0x04UL)
#define RK_MPU_ATTR_INDEX_NORMAL (0U)
#define RK_MPU_ATTR_INDEX_DEVICE (1U)

/* Compact RK01 attribute descriptor consumed by kMpuBuildRegion(). */
#define RK_MPU_ATTR_DESC(ap, sh, xn, attrIdx)                                \
    ((((ULONG)(ap) & 0x3UL) << 0U) |                                        \
     (((ULONG)(sh) & 0x3UL) << 2U) |                                        \
     (((ULONG)(xn) & 0x1UL) << 4U) |                                        \
     (((ULONG)(attrIdx) & 0x7UL) << 5U))
#define RK_MPU_ATTR_DESC_AP(value) (((ULONG)(value) >> 0U) & 0x3UL)
#define RK_MPU_ATTR_DESC_SH(value) (((ULONG)(value) >> 2U) & 0x3UL)
#define RK_MPU_ATTR_DESC_XN(value) (((ULONG)(value) >> 4U) & 0x1UL)
#define RK_MPU_ATTR_DESC_INDEX(value) (((ULONG)(value) >> 5U) & 0x7UL)

/* User FLASH is executable and read-only to both privileged and unprivileged
 * code. */
#define RK_MPU_ATTR_USER_FLASH                                               \
    RK_MPU_ATTR_DESC(RK_MPU_AP_READ_ONLY, RK_MPU_SH_NON_SHAREABLE, 0U,      \
                     RK_MPU_ATTR_INDEX_NORMAL)

/* User SRAM is read/write, inner-shareable, normal non-cacheable and XN. */
#define RK_MPU_ATTR_USER_SRAM                                                \
    RK_MPU_ATTR_DESC(RK_MPU_AP_FULL_ACCESS, RK_MPU_SH_INNER_SHAREABLE, 1U,  \
                     RK_MPU_ATTR_INDEX_NORMAL)

/* Peripheral windows should be data-only device memory. */
#define RK_MPU_ATTR_USER_PERIPHERAL                                          \
    RK_MPU_ATTR_DESC(RK_MPU_AP_FULL_ACCESS, RK_MPU_SH_NON_SHAREABLE, 1U,    \
                     RK_MPU_ATTR_INDEX_DEVICE)

/* User-deny region, useful for future guard holes or subregion schemes. */
#define RK_MPU_ATTR_NO_ACCESS                                                \
    RK_MPU_ATTR_DESC(RK_MPU_AP_PRIV_RW, RK_MPU_SH_NON_SHAREABLE, 1U,        \
                     RK_MPU_ATTR_INDEX_NORMAL)

/* Precomputed hardware region values. TCBs store these so context switch only
 * writes RBAR/RLAR and does not recalculate MPU encoding. */
typedef struct RK_STRUCT_MPU_REGION
{
    ULONG rbar;
    ULONG rlar;
} RK_MPU_REGION;

/* Hardware-stacked exception frame for Cortex-M exception entry without FPU
 * context. SVC and MemManage handlers pass this to C code. */
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
