/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                  */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                              */
/*                                                                            */
/******************************************************************************/

#ifndef RK_API_H
#define RK_API_H

#include <kexecutive.h>

/******************************************************************************/
/**
 * @brief              Initialise a new task in the implicit application domain.
 *                     This is the RK0-shaped default: tasks created with
 *                     kTaskInit() share the App-domain RAM view while still
 *                     using syscall-backed kernel services. Task prototype:
 *
 *                     VOID taskFunc(VOID *args)
 *
 *                     (See RK_DECLARE_TASK convenience macro)
 *
 * @param taskHandlePtr Pointer to the Handle object for the task.
 *
 * @param taskFunc     Task's entry function.
 *
 * @param argsPtr      Pointer to initial task arguments.
 *
 * @param taskName     Task name. Standard size is 8 bytes.
 *                     (RK_OBJ_MAX_NAME_LEN)
 *
 * @param stackBufPtr     Pointer to the task stack (the array's name).
 *                        Must be declared with RK_DECLARE_TASK() or placed in
 *                        RK task-stack RAM. Must have MPU region geometry and
 *                        must not be NULL.
 *
 * @param stackSize    Size of the task stack, in words. Must be at least
 *                     RK_CONF_MIN_STACKSIZE and even, so the initial stack frame
 *                     remains 8-byte aligned.
 *
 *
 * @param priority     Task priority - valid range: 0-31.(0 is highest).
 *                     Priority 0 is legal for application tasks, but it is
 *                     also used by PostProcSysTask. Equal-priority readiness
 *                     does not preempt the running task, so long-running
 *                     priority-0 application code can delay callouts and
 *                     deferred wake processing.
 *
 * @param preempt   Scheduling mode for this task:
 *                  RK_PREEMPT or RK_NO_PREEMPT only.
 *
 *                  If RK_NO_PREEMPT is selected, once dispatched the task
 *                  will not be preempted by user tasks until it blocks,
 *                  yields, or otherwise leaves RUNNING.
 *                  Non-preemptible tasks are typically used for short,
 *                  bounded service routines.
 *
 * @return
 *                  RK_ERR_SUCCESS            Task created.
 *                  RK_ERR_OBJ_NULL           Any required pointer is NULL.
 *                  RK_ERR_INVALID_ISR_PRIMITIVE
 *                                              Called from ISR context.
 *                  RK_ERR_INVALID_PARAM      Invalid stack size or preempt mode.
 *                  RK_ERR_INVALID_PRIO       Priority is out of range.
 *                  RK_ERR_TASK_POOL_EMPTY    No free TCB slot in the task pool.
 *                  RK_ERR_ERROR              Internal failure creating the task.
 *
 *
 */

RK_ERR kTaskInit(RK_TASK_HANDLE *taskHandlePtr, const RK_TASKENTRY taskFunc,
                 VOID *argsPtr, RK_STRING taskName,
                 RK_STACK *const stackBufPtr, const ULONG stackSize,
                 const RK_PRIO priority, const RK_OPTION preempt);
#define kCreateTask kTaskInit /* alias*/
#define kTaskCreate kTaskInit

/* kTaskInit() is the RK0-shaped default: every task created through it joins
 * the implicit application domain and therefore shares that domain's writable
 * application RAM with the other default tasks. Use kTaskInitDomain() for an
 * explicit domain and kTaskInitIsolated() only when a task must get its own
 * one-task domain. */

/**
 * @brief Return RK01's implicit application domain after it has been created.
 *        Normal code usually does not need this pointer; plain Create APIs bind
 *        boot-created domain-local objects to this domain automatically.
 */
RK_DOMAIN *kApplicationDomainGet(VOID);

/**
 * @brief Initialise a statically declared writable-authority boundary shared
 *        by one or more tasks. Tasks created in the same domain can read/write
 *        this memory while tasks outside the domain cannot.
 *
 *        Domain memory must be power-of-two sized, naturally aligned to that
 *        size, and placed in RK task RAM.
 */
RK_ERR kDomainInit(RK_DOMAIN *const domainPtr,
                   BYTE *const regionBasePtr,
                   ULONG const regionBytes,
                   RK_STRING domainName);

/**
 * @brief Allocate bytes from a domain RAM window during BOOT construction.
 *        The returned memory belongs to the domain and remains valid for the
 *        lifetime of that domain. Allocation closes once tasks have been
 *        created in the domain or the MPU layout has been finalised.
 */
VOID *kDomainAlloc(RK_DOMAIN *const domainPtr,
                   ULONG const nBytes,
                   ULONG const alignBytes);

/**
 * @brief Low-level MPU shared-region descriptor initialisation.
 *        Prefer kSharedMemCreate()/kSharedMemAttach() in application code.
 */
RK_ERR kSharedRegionInit(RK_SHARED_REGION *const regionPtr,
                         BYTE *const regionBasePtr,
                         ULONG const regionBytes);

/**
 * @brief Low-level MPU shared-region mapping.
 *        Prefer kSharedMemAttach() in application code. Call before creating
 *        tasks in that domain.
 */
RK_ERR kDomainMapSharedRegion(RK_DOMAIN *const domainPtr,
                              RK_SHARED_REGION *const regionPtr);

/**
 * @brief Create a handle-addressed inter-domain shared memory segment.
 *        The backing RAM must have normal MPU region geometry and live in
 *        task RAM. This is a construction-phase service; attach domains before
 *        creating tasks in those domains.
 * @param objName NUL-terminated object name.
 */
RK_ERR kSharedMemCreate(RK_SHARED_MEM_HANDLE *const sharedMemHandlePtr,
                        RK_STRING objName,
                        VOID *const regionBasePtr,
                        ULONG const regionBytes);

/**
 * @brief Attach a shared memory segment to a domain. A shared segment is useful
 *        only across domain boundaries; same-domain tasks should use their
 *        domain RAM instead.
 */
RK_ERR kSharedMemAttach(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                        RK_DOMAIN *const domainPtr);

/**
 * @brief Remove a construction-phase shared memory attachment from a domain.
 *        This is for BOOT rollback before task creation and MPU layout
 *        finalisation, not runtime unmapping.
 */
RK_ERR kSharedMemDetach(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                        RK_DOMAIN *const domainPtr);

/**
 * @brief Destroy an unattached shared memory segment before MPU layout
 *        finalisation.
 */
RK_ERR kSharedMemDestroy(RK_SHARED_MEM_HANDLE *const sharedMemHandlePtr);

/**
 * @brief Return the shared segment address and size to an attached domain.
 *        Runtime callers must belong to a domain attached with
 *        kSharedMemAttach(). The service rejects single-domain use.
 */
RK_ERR kSharedMemGet(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                     VOID **const regionBasePPtr,
                     ULONG *const regionBytesPtr);

/**
 * @brief Initialise a task that belongs to a domain. The task keeps its own
 *        scheduler identity while its MPU view includes the domain.
 *        Its stack must be a private MPU-shaped task-stack region outside
 *        domainPtr's RAM. Tasks in the same domain share only the domain RAM
 *        and explicit grants, not each other's stacks.
 */
RK_ERR kTaskInitDomain(RK_TASK_HANDLE *taskHandlePtr,
                       const RK_TASKENTRY taskFunc,
                       VOID *argsPtr,
                       RK_STRING taskName,
                       RK_STACK *const stackBufPtr,
                       const ULONG stackSize,
                       const RK_PRIO priority,
                       const RK_OPTION preempt,
                       RK_DOMAIN *const domainPtr);
#define kTaskCreateIn kTaskInitDomain

/**
 * @brief Initialise a deliberately isolated unprivileged task. The task gets a
 *        private one-task domain using only the provided stack buffer as its
 *        RAM region. The stack must therefore have MPU region geometry: a
 *        power-of-two byte size and matching natural alignment.
 */
RK_ERR kTaskInitIsolated(RK_TASK_HANDLE *taskHandlePtr,
                         const RK_TASKENTRY taskFunc,
                         VOID *argsPtr,
                         RK_STRING taskName,
                         RK_STACK *const stackBufPtr,
                         const ULONG stackSize,
                         const RK_PRIO priority,
                         const RK_OPTION preempt);
#define kTaskCreateIsolated kTaskInitIsolated

/**
 * @brief Initialise an unprivileged protected task from an explicit memory
 *        arena. If memoryPtr->domainPtr is NULL, the arena becomes a private
 *        isolated domain owned by this task. If domainPtr is set, the task
 *        joins that domain and its stack must be a private task-stack region
 *        outside the domain RAM.
 *
 *        A private isolated domain may be identical to the task stack:
 *        regionBasePtr == stackBasePtr and regionBytes == stackWords *
 *        sizeof(RK_STACK).
 *
 * @param taskPtr      Privileged TCB storage for the task.
 * @param taskFunc     Task entry function.
 * @param argsPtr      Task argument pointer. For isolated mutable state, point
 *                     this into memoryPtr->regionBasePtr.
 * @param priority     Task priority.
 * @param memoryPtr    Task memory arena and stack description.
 *
 * @return RK_ERR_SUCCESS, RK_ERR_OBJ_NULL, RK_ERR_INVALID_ISR_PRIMITIVE,
 *         RK_ERR_INVALID_PARAM, RK_ERR_INVALID_PRIO, RK_ERR_INVALID_OBJ, or
 *         RK_ERR_TASK_POOL_EMPTY.
 */
RK_ERR kTaskInitProtected(RK_TCB *const taskPtr,
                          RK_TASKENTRY const taskFunc,
                          VOID *const argsPtr,
                          RK_PRIO const priority,
                          RK_TASK_MEMORY const *const memoryPtr);
#if (RK_CONF_DYNAMIC_TASK == ON)
/**
 * @brief Spawn a runtime task using the shared task pool and a user-selected
 *        stack partition.
 *        The spawned task stack size is the partition block size (in words).
 *        If domainPtr is NULL, the new task joins the caller's domain at
 *        runtime or the implicit App domain during BOOT. If domainPtr is set,
 *        it must name an already-finalised domain after layout finalisation.
 *        Controlled by RK_CONF_DYNAMIC_TASK in kconfig.h.
 * @param taskAttrPtr Pointer to dynamic task attributes.
 *                     stackMemPtr must identify the stack-block partition.
 *                     domainPtr is the domain the spawned task joins under MPU,
 *                     or NULL to inherit the caller/App domain.
 * @param taskHandlePtr Receives task handle.
 * @return
 *                  RK_ERR_SUCCESS            Task spawned.
 *                  RK_ERR_OBJ_NULL           Required attribute pointer is NULL.
 *                  RK_ERR_INVALID_ISR_PRIMITIVE
 *                                              Called from ISR context.
 *                  RK_ERR_INVALID_PARAM      Invalid preempt mode or invalid
 *                                              partition block geometry.
 *                  RK_ERR_INVALID_PRIO       Priority is out of range.
 *                  RK_ERR_INVALID_OBJ        `stackMemPtr` is not a valid
 *                                              initialised memory partition.
 *                  RK_ERR_OBJ_NOT_INIT       `domainPtr` is not initialised.
 *                  RK_ERR_TASK_POOL_EMPTY    No free stack block in partition
 *                                              or no free TCB in task pool.
 *                  RK_ERR_ERROR              Internal failure creating the task.
 */
RK_ERR kTaskSpawn(RK_DYNAMIC_TASK_ATTR const *taskAttrPtr,
                  RK_TASK_HANDLE *taskHandlePtr);
#endif

#if (RK_CONF_DYNAMIC_TASK == ON)
/**
 * @brief Terminate a dynamic task and return its resources to the pools.
 *        If the running task terminates itself, the operation is deferred to
 *        PostProc and the caller is pended for a context switch.
 * @param taskHandlePtr Address of a task handle variable.
 *                      On success, *taskHandlePtr is set to NULL.
 * @return
 *                  RK_ERR_SUCCESS            Task terminated.
 *                  RK_ERR_OBJ_NULL           Handle pointer is NULL.
 *                  RK_ERR_TASK_POOL_NOT_INIT Task pool not initialised.
 *                  RK_ERR_INVALID_ISR_PRIMITIVE
 *                                              Called from ISR context.
 *                  RK_ERR_OBJ_NOT_INIT       Target task is not initialised.
 *                  RK_ERR_INVALID_OBJ        System task, static task, or
 *                                              invalid object.
 *                  RK_ERR_TASK_INVALID_ST    Target task cannot be terminated
 *                                              in its current state.
 *                  RK_ERR_NOWAIT             Deferred terminate queue full.
 */
RK_ERR kTaskTerminate(RK_TASK_HANDLE *taskHandlePtr);


/**
 * @brief Terminate the caller dynamic task using deferred self-termination
 *        semantics.
 * @return
 *                  RK_ERR_SUCCESS            Caller accepted termination.
 *                  RK_ERR_INVALID_ISR_PRIMITIVE
 *                                              Called from ISR context.
 *                  RK_ERR_INVALID_OBJ        Caller is invalid, system task,
 *                                              or static task.
 *                  Plus all outputs from kTaskTerminate() for the caller task.
 */
RK_ERR kTaskTerminateSelf(VOID);
#endif

/**
 * @brief Initialise kernel-owned dynamic object partitions.
 *        Each enabled runtime object family owns a fixed partition pool with
 *        RK_CONF_DYNAMIC_*_MAX slots. Normal startup calls this from kInit()
 *        before kApplicationInit(); application constructors can then call
 *        kSemaphoreCreate(), kMutexCreate(), kSleepQueueCreate(),
 *        kMesgQueueCreate(), kTimerCreate(), or kMRMCreate() directly. The
 *        handle variable passed to any Create API must contain NULL before the
 *        call. Use the GlobalScope or DomainScope constructors during
 *        BOOT/configuration when object ownership must be explicit before tasks
 *        run.
 * @return RK_ERR_SUCCESS on success, or a propagated partition init error.
 */
RK_ERR kObjPartitionsInit(VOID);

/*
 * Object scope policy:
 *   - RK_SCOPE_DOMAIN_LOCAL: shared-state service used only by tasks in the
 *     named domain.
 *   - RK_SCOPE_KERNEL_GLOBAL: explicit kernel/global object, normally reserved
 *     for trusted services such as RK_SHARED_MEM wrappers and the logger.
 *   - RK_SCOPE_UNASSIGNED: internal placeholder for objects that have not yet
 *     been bound to a usable task/domain scope.
 */

#ifndef RK_STACK_ALIGN
#define RK_STACK_ALIGN(NWORDS) K_ALIGN((NWORDS) * sizeof(RK_STACK))
#endif

#ifndef RK_APP_RAM_ATTR
#define RK_APP_RAM_ATTR K_ALIGN(4) RK_SECTION_APP_RAM
#endif

#ifndef RK_SHARED_APP
#define RK_SHARED_APP RK_APP_RAM_ATTR
#endif

#ifndef RK_TASK_STACK_ALIGN_ATTR
#define RK_TASK_STACK_ALIGN_ATTR(NWORDS) RK_STACK_ALIGN(NWORDS)
#endif

#ifndef RK_TASK_STACK_ATTR
#define RK_TASK_STACK_ATTR(NWORDS)                                            \
    RK_STACK_ALIGN(NWORDS) RK_SECTION_TASK_STACK
#endif

#ifndef RK_ISOLATED_TASK_STACK_ATTR
#define RK_ISOLATED_TASK_STACK_ATTR(NWORDS)                                   \
    RK_STACK_ALIGN(NWORDS) RK_SECTION_TASK_STACK
#endif

#ifndef RK_PRIVILEGED_TASK_STACK_ATTR
#define RK_PRIVILEGED_TASK_STACK_ATTR(NWORDS) K_ALIGN(8)
#endif

#ifndef RK_DOMAIN_RAM_ATTR
#define RK_DOMAIN_RAM_ATTR(NBYTES) K_ALIGN(NBYTES) RK_SECTION_DOMAIN_RAM
#endif

#ifndef RK_KERNEL_RAM_ATTR
#define RK_KERNEL_RAM_ATTR K_ALIGN(4) RK_SECTION_NAMED(".rk_kernel_bss")
#endif

#ifndef RK_DOMAIN_DESC_ATTR
#define RK_DOMAIN_DESC_ATTR RK_KERNEL_RAM_ATTR
#endif

#ifndef RK_SHARED_REGION_ATTR
#define RK_SHARED_REGION_ATTR(NBYTES) K_ALIGN(NBYTES) RK_SECTION_DOMAIN_RAM
#endif

#ifndef RK_SHARED_RAM_ATTR
#define RK_SHARED_RAM_ATTR RK_SECTION_SHARED_BSS
#endif

#ifndef RK_RETAINED_TASK_RAM_ATTR
#define RK_RETAINED_TASK_RAM_ATTR RK_SECTION_TASK_NOINIT
#endif

#ifndef RK_RETAINED_SHARED_RAM_ATTR
#define RK_RETAINED_SHARED_RAM_ATTR RK_SECTION_SHARED_NOINIT
#endif

#ifndef RK_TASK_HANDLE_ATTR
#define RK_TASK_HANDLE_ATTR RK_SHARED_RAM_ATTR
#endif

#ifndef RK_KOBJ_HANDLE_ATTR
#define RK_KOBJ_HANDLE_ATTR RK_SHARED_RAM_ATTR
#endif

/**
 * @brief Declare runtime kernel-object handle variables.
 *
 *        Declaration scope controls where the handle token is stored. It does
 *        not create the object and it does not decide the kernel object's
 *        visibility. Pair the declaration with the matching Create API:
 *        k...Create(), k...CreateGlobalScope() or k...CreateDomainScope().
 *
 *        Local handle variables live in the implicit App RAM aperture. Global
 *        handle variables live in shared RAM so explicit domain tasks can read
 *        the token.
 */
#ifndef RK_DECLARE_LOCAL_KOBJ_HANDLE
#define RK_DECLARE_LOCAL_KOBJ_HANDLE(HANDLE)                                   \
    RK_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_KOBJ_HANDLE
#define RK_DECLARE_GLOBAL_KOBJ_HANDLE(HANDLE)                                  \
    RK_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_KOBJ_HANDLE
#define RK_DECLARE_KOBJ_HANDLE(HANDLE) RK_DECLARE_GLOBAL_KOBJ_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_TASK_HANDLE
#define RK_DECLARE_LOCAL_TASK_HANDLE(HANDLE)                                   \
    RK_TASK_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_TASK_HANDLE
#define RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)                                  \
    RK_TASK_HANDLE HANDLE RK_TASK_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_TASK_HANDLE
#define RK_DECLARE_TASK_HANDLE(HANDLE) RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_SEMAPHORE
#define RK_DECLARE_LOCAL_SEMAPHORE(HANDLE)                                     \
    RK_SEMAPHORE_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_SEMAPHORE
#define RK_DECLARE_GLOBAL_SEMAPHORE(HANDLE)                                    \
    RK_SEMAPHORE_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_SEMAPHORE
#define RK_DECLARE_SEMAPHORE(HANDLE) RK_DECLARE_GLOBAL_SEMAPHORE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_MUTEX
#define RK_DECLARE_LOCAL_MUTEX(HANDLE)                                         \
    RK_MUTEX_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MUTEX
#define RK_DECLARE_GLOBAL_MUTEX(HANDLE)                                        \
    RK_MUTEX_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_MUTEX
#define RK_DECLARE_MUTEX(HANDLE) RK_DECLARE_GLOBAL_MUTEX(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_SLEEP_QUEUE
#define RK_DECLARE_LOCAL_SLEEP_QUEUE(HANDLE)                                   \
    RK_SLEEP_QUEUE_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_SLEEP_QUEUE
#define RK_DECLARE_GLOBAL_SLEEP_QUEUE(HANDLE)                                  \
    RK_SLEEP_QUEUE_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_SLEEP_QUEUE
#define RK_DECLARE_SLEEP_QUEUE(HANDLE) RK_DECLARE_GLOBAL_SLEEP_QUEUE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE
#define RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE(HANDLE)                             \
    RK_MESG_QUEUE_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE
#define RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE(HANDLE)                            \
    RK_MESG_QUEUE_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_MESG_QUEUE_HANDLE
#define RK_DECLARE_MESG_QUEUE_HANDLE(HANDLE)                                   \
    RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_MBOX_HANDLE
#define RK_DECLARE_LOCAL_MBOX_HANDLE(HANDLE)                                   \
    RK_MBOX_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MBOX_HANDLE
#define RK_DECLARE_GLOBAL_MBOX_HANDLE(HANDLE)                                  \
    RK_MBOX_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_MBOX_HANDLE
#define RK_DECLARE_MBOX_HANDLE(HANDLE) RK_DECLARE_GLOBAL_MBOX_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_TIMER
#define RK_DECLARE_LOCAL_TIMER(HANDLE)                                         \
    RK_TIMER_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_TIMER
#define RK_DECLARE_GLOBAL_TIMER(HANDLE)                                        \
    RK_TIMER_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_TIMER
#define RK_DECLARE_TIMER(HANDLE) RK_DECLARE_GLOBAL_TIMER(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_MRM
#define RK_DECLARE_LOCAL_MRM(HANDLE)                                           \
    RK_MRM_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_MRM
#define RK_DECLARE_GLOBAL_MRM(HANDLE)                                          \
    RK_MRM_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_MRM
#define RK_DECLARE_MRM(HANDLE) RK_DECLARE_GLOBAL_MRM(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_SHARED_MEM_HANDLE
#define RK_DECLARE_LOCAL_SHARED_MEM_HANDLE(HANDLE)                             \
    RK_SHARED_MEM_HANDLE HANDLE RK_APP_RAM_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_GLOBAL_SHARED_MEM_HANDLE
#define RK_DECLARE_GLOBAL_SHARED_MEM_HANDLE(HANDLE)                            \
    RK_SHARED_MEM_HANDLE HANDLE RK_KOBJ_HANDLE_ATTR = RK_NULL_HANDLE;
#endif

#ifndef RK_DECLARE_SHARED_MEM_HANDLE
#define RK_DECLARE_SHARED_MEM_HANDLE(HANDLE)                                   \
    RK_DECLARE_GLOBAL_SHARED_MEM_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_LOCAL_MEM_PARTITION
#define RK_DECLARE_LOCAL_MEM_PARTITION(PARTITION)                              \
    RK_MEM_PARTITION PARTITION RK_APP_RAM_ATTR;
#endif

#ifndef RK_DECLARE_GLOBAL_MEM_PARTITION
#define RK_DECLARE_GLOBAL_MEM_PARTITION(PARTITION)                             \
    RK_MEM_PARTITION PARTITION K_ALIGN(4) RK_SHARED_RAM_ATTR;
#endif

#ifndef RK_DECLARE_MEM_PARTITION
#define RK_DECLARE_MEM_PARTITION(PARTITION)                                    \
    RK_DECLARE_LOCAL_MEM_PARTITION(PARTITION)
#endif

/**
 * @brief Declare data needed to create a task
 * @param HANDLE Task Handle
 * @param TASKENTRY Task's entry function
 * @param STACKBUF  Array's name for the task's stack
 * @param NWORDS    Stack Size in number of WORDS (even)
 */
#ifndef RK_DECLARE_TASK
#define RK_DECLARE_TASK(HANDLE, TASKENTRY, STACKBUF, NWORDS)                   \
    VOID TASKENTRY(VOID *args);                                                \
    RK_STACK STACKBUF[NWORDS] RK_TASK_STACK_ATTR(NWORDS);                      \
    RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_ISOLATED_TASK
#define RK_DECLARE_ISOLATED_TASK(HANDLE, TASKENTRY, STACKBUF, NWORDS)          \
    VOID TASKENTRY(VOID *args);                                                \
    RK_STACK STACKBUF[NWORDS] RK_ISOLATED_TASK_STACK_ATTR(NWORDS);             \
    RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

/**
 * @brief Declare a domain descriptor and a raw byte MPU-shaped RAM block.
 */
#ifndef RK_DECLARE_DOMAIN
#define RK_DECLARE_DOMAIN(DOMAIN, RAMBUF, NBYTES)                              \
    BYTE RAMBUF[NBYTES] RK_DOMAIN_RAM_ATTR(NBYTES);                            \
    RK_DOMAIN DOMAIN RK_DOMAIN_DESC_ATTR;
#endif

#ifndef RK_DECLARE_DOMAIN_RAM
#define RK_DECLARE_DOMAIN_RAM(RAM_LAYOUT, ...)                                  \
    typedef struct                                                              \
    {                                                                          \
        __VA_ARGS__                                                            \
    } RAM_LAYOUT;
#endif

#ifndef RK_DOMAIN_RAM_MEMBER
#define RK_DOMAIN_RAM_MEMBER(TYPE, NAME) TYPE NAME;
#endif

#ifndef RK_DOMAIN_RAM_ARRAY
#define RK_DOMAIN_RAM_ARRAY(TYPE, NAME, COUNT) TYPE NAME[COUNT];
#endif

#ifndef RK_DOMAIN_RAM_TASK_HANDLE
#define RK_DOMAIN_RAM_TASK_HANDLE(NAME) RK_TASK_HANDLE NAME;
#endif

#ifndef RK_DOMAIN_RAM_MUTEX_HANDLE
#define RK_DOMAIN_RAM_MUTEX_HANDLE(NAME) RK_MUTEX_HANDLE NAME;
#endif

#ifndef RK_DOMAIN_RAM_SEMAPHORE_HANDLE
#define RK_DOMAIN_RAM_SEMAPHORE_HANDLE(NAME) RK_SEMAPHORE_HANDLE NAME;
#endif

#ifndef RK_DOMAIN_WINDOW_STATIC_ASSERT_
#define RK_DOMAIN_WINDOW_STATIC_ASSERT_(RAM_LAYOUT, NBYTES)                    \
    _Static_assert(sizeof(RAM_LAYOUT) <= (NBYTES),                             \
                   "domain RAM layout must fit its MPU window");              \
    _Static_assert((NBYTES) >= 32U,                                            \
                   "domain RAM window must be at least 32 bytes");            \
    _Static_assert(((NBYTES) & ((NBYTES) - 1U)) == 0U,                         \
                   "domain RAM window must be a power of two");               \
    _Static_assert(_Alignof(RAM_LAYOUT) <= (NBYTES),                           \
                   "domain RAM layout alignment must fit its MPU window")
#endif

#ifndef RK_DECLARE_TYPED_DOMAIN
#define RK_DECLARE_TYPED_DOMAIN(DOMAIN, RAMBUF, RAM_LAYOUT, NBYTES)            \
    RK_DOMAIN_WINDOW_STATIC_ASSERT_(RAM_LAYOUT, NBYTES);                       \
    typedef union                                                              \
    {                                                                          \
        BYTE bytes[NBYTES];                                                    \
        RAM_LAYOUT typed;                                                      \
    } RAMBUF##_RK_DOMAIN_WINDOW;                                               \
    RAMBUF##_RK_DOMAIN_WINDOW RAMBUF RK_DOMAIN_RAM_ATTR(NBYTES);               \
    RK_DOMAIN DOMAIN RK_DOMAIN_DESC_ATTR;
#endif

#ifndef RK_DOMAIN_WINDOW_BASE
#define RK_DOMAIN_WINDOW_BASE(RAMBUF) ((BYTE *)(VOID *)&(RAMBUF))
#endif

#ifndef RK_DOMAIN_WINDOW_BYTES
#define RK_DOMAIN_WINDOW_BYTES(RAMBUF) ((ULONG)sizeof(RAMBUF))
#endif

#ifndef RK_DOMAIN_STATE
#define RK_DOMAIN_STATE(RAMBUF) (&((RAMBUF).typed))
#endif

#ifndef RK_DOMAIN_INIT_TYPED
#define RK_DOMAIN_INIT_TYPED(DOMAINPTR, RAMBUF, NAME)                         \
    kDomainInit((DOMAINPTR), RK_DOMAIN_WINDOW_BASE(RAMBUF),                   \
                RK_DOMAIN_WINDOW_BYTES(RAMBUF), (NAME))
#endif

#ifndef RK_DOMAIN_ALLOC
#define RK_DOMAIN_ALLOC(DOMAINPTR, TYPE)                                       \
    ((TYPE *)kDomainAlloc((DOMAINPTR), sizeof(TYPE), (ULONG)_Alignof(TYPE)))
#endif

#ifndef RK_DOMAIN_ALLOC_ARRAY
#define RK_DOMAIN_ALLOC_ARRAY(DOMAINPTR, TYPE, COUNT)                         \
    ((TYPE *)kDomainAlloc((DOMAINPTR),                                        \
                          sizeof(TYPE) * (ULONG)(COUNT),                      \
                          (ULONG)_Alignof(TYPE)))
#endif

/**
 * @brief Declare a low-level inter-domain shared region object and RAM block.
 *        Prefer RK_DECLARE_SHARED_MEM() in application code.
 */
#ifndef RK_DECLARE_SHARED_REGION
#define RK_DECLARE_SHARED_REGION(REGION, RAMBUF, NBYTES)                       \
    BYTE RAMBUF[NBYTES] RK_SHARED_REGION_ATTR(NBYTES);                         \
    RK_SHARED_REGION REGION;
#endif

/**
 * @brief Declare an inter-domain shared memory handle and MPU-shaped backing
 *        byte RAM for kSharedMemCreate().
 */
#ifndef RK_DECLARE_SHARED_MEM_RAW
#define RK_DECLARE_SHARED_MEM_RAW(HANDLE, RAMBUF, NBYTES)                      \
    BYTE RAMBUF[NBYTES] RK_SHARED_REGION_ATTR(NBYTES);                         \
    RK_DECLARE_GLOBAL_SHARED_MEM_HANDLE(HANDLE)
#endif

/**
 * @brief Declare a shared-memory handle and a C struct placed as backing RAM.
 */
#ifndef RK_DECLARE_SHARED_MEM_STRUCT
#define RK_DECLARE_SHARED_MEM_STRUCT(HANDLE, RAMOBJ, RAM_LAYOUT, NBYTES)       \
    RAM_LAYOUT RAMOBJ RK_SHARED_REGION_ATTR(NBYTES);                           \
    RK_DECLARE_GLOBAL_SHARED_MEM_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_SHARED_MEM_SELECT_
#define RK_DECLARE_SHARED_MEM_SELECT_(_1, _2, _3, _4, NAME, ...) NAME
#endif

#ifndef RK_DECLARE_SHARED_MEM
#define RK_DECLARE_SHARED_MEM(...)                                             \
    RK_DECLARE_SHARED_MEM_SELECT_(__VA_ARGS__, RK_DECLARE_SHARED_MEM_STRUCT,   \
                                  RK_DECLARE_SHARED_MEM_RAW)(__VA_ARGS__)
#endif

/**
 * @brief Declare a domain member task handle.
 */
#ifndef RK_DECLARE_DOMAIN_TASK
#define RK_DECLARE_DOMAIN_TASK(HANDLE, TASKENTRY)                              \
    VOID TASKENTRY(VOID *args);                                                \
    RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

/**
 * @brief Declare a private stack for a domain member task.
 */
#ifndef RK_DECLARE_DOMAIN_TASK_STACK
#define RK_DECLARE_DOMAIN_TASK_STACK(STACKBUF, NWORDS)                         \
    RK_STACK STACKBUF[NWORDS] RK_TASK_STACK_ATTR(NWORDS);
#endif

/**
 * @brief Declare a dynamic task handle (no static stack buffer).
 */
#ifndef RK_DECLARE_DYNAMIC_TASK
#define RK_DECLARE_DYNAMIC_TASK(HANDLE, TASKENTRY)                              \
    VOID TASKENTRY(VOID *args);                                                \
    RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

/**
 * @brief Declare a dynamic task stack partition storage.
 */
#ifndef RK_DECLARE_DYNAMIC_STACK_POOL
#define RK_DECLARE_DYNAMIC_STACK_POOL(PARTITION, STACKBUF, NBLOCKS, NWORDS)     \
    RK_DECLARE_LOCAL_MEM_PARTITION(PARTITION)                                   \
    RK_STACK STACKBUF[NBLOCKS][NWORDS] RK_TASK_STACK_ATTR(NWORDS);
#endif

/**
 * @brief Initialises the kernel. To be called in main()
 *        after hardware initialisation.
 */
VOID kInit(VOID);

/**
 * @brief Yields the current task.
 *        Note, the highest priority task should be RUNNING.
 *        Yielding is meaningful for FIFO discipline among
 *        tasks of with the same priority.
 */
VOID kYield(VOID);
/**
 * @brief  Returns the handle of the currently running task.
 * @return Task handle of the caller.
 */
RK_TASK_HANDLE kTaskGetRunningHandle(VOID);

/**
 * @brief  Returns the name of the currently running task (pointer).
 * @return Const pointer to task name string. In MPU thread mode, the pointer
 *         refers to a shared read/write diagnostic snapshot, not the TCB field.
 */
const CHAR *kTaskGetRunningName(VOID);

/**
 * @brief  Retrieves a task's TID.
 * @param  taskHandle Target task handle.
 * @return TID of the task.
 */
RK_TID kTaskGetID(RK_TASK_HANDLE taskHandle);


/**
 * @brief  Copies a task's name into the provided buffer.
 * @param  taskHandle Target task handle.
 * @param  buf        Destination buffer (size >= RK_OBJ_MAX_NAME_LEN).
 * @return RK_ERR_SUCCESS on copy, RK_ERR_OBJ_NULL if params are NULL.
 */
RK_ERR kTaskGetName(RK_TASK_HANDLE taskHandle, CHAR *buf);

/**
 * @brief  Returns a task's current priority.
 * @param  taskHandle Target task handle.
 * @return Priority of the task.
 */
RK_PRIO kTaskGetPrio(RK_TASK_HANDLE taskHandle);

/**
 * @brief  Returns a task's nominal priority.
 * @param  taskHandle Target task handle.
 * @return Nominal priority of the task.
 */
RK_PRIO kTaskGetNomPrio(RK_TASK_HANDLE taskHandle);

/******************************************************************************/
/*PREEMPT DISABLE/ENABLE*/
/******************************************************************************/
/**
 * @brief Locks the scheduler so the current task cannot be preempted by another
 *        user task. Locks are nested.
 */
extern VOID kSchLock(VOID);
#ifndef kPreemptDisable
#define kPreemptDisable kSchLock
#endif
/**
 * @brief Unlocks the scheduler. If the number of nested locks is 0, any delayed
 *        task switching happens immediately after unlocking.
 */
extern VOID kSchUnlock(VOID);
#ifndef kPreemptEnable
#define kPreemptEnable kSchUnlock
#endif

/******************************************************************************/
/* TASK'S EVENT REGISTER (EVENT FLAGS)                                        */
/******************************************************************************/
/**
 * @brief               A task check for events set on its
 *                        event register.
 * @param required      Events required a bitstring (flags)
 *
 * @param options       RK_EVENT_ANY - any of the required event flags
 *                      satisfies the waiting condition if set.
 *                      RK_EVENT_ALL - all required flags need to be set
 *                      to satisfy the waiting condition.
 *
 * @param gotFlagsPtr    Pointer to RK_TASK_EVENT to store the state of the
 *                       flags when condition is met, before they are cleared.
 *                      (opt. NULL)
 *
 * @param timeout       Waiting time until condition is met.
 *
 * @return              Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsucessful:
 *                                   RK_ERR_FLAGS_NOT_MET
 *                                   RK_ERR_TIMEOUT
 *                                   RK_ERR_INVALID_TIMEOUT
 *                      Errors:
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 *                                   RK_ERR_INVALID_PARAM
 */
RK_ERR kEventGet(RK_TASK_EVENT const required, RK_OPTION const options,
                 RK_TASK_EVENT *const gotFlagsPtr, RK_TICK timeout);
/**
 * @brief             Post a combination of event flags to a task.
 *                    This combination is OR'ed to the current flags.
 *
 * @param taskHandle    Receiver Task handle
 *
 * @param mask         Bitmask to be OR'ed (0UL is invalid)
 *
 * @return
 *                     Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                  RK_ERR_OBJ_NULL
 *                                  RK_ERR_INVALID_PARAM
 */
RK_ERR kEventSet(RK_TASK_HANDLE const taskHandle, RK_TASK_EVENT const mask);

/**
 * @brief                   Retrieves current event register state of a task
 *
 * @param taskHandle    Handle of the Target task.
 *                    If NULL the target is the caller. (error if on an ISR)
 *
 * @param gotFlagsPtr   Pointer to store the current events
 * @return              Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kEventQuery(RK_TASK_HANDLE const taskHandle,
                   RK_TASK_EVENT *const gotFlagsPtr);
/**
 * @brief Clears specified flags
 * @param taskHandle   Target task. NULL sets the target as the caller task.
 * @param flagsToClear Positions to clear. 0UL is invalid.
 * @return              Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 *                                   (if taskHandle == NULL)
 *                                   RK_ERR_INVALID_PARAM
 *
 */
RK_ERR kEventClear(RK_TASK_HANDLE const taskHandle,
                   RK_TASK_EVENT const flagsToClear);

/******************************************************************************/
/* SEMAPHORES (COUNTING/BINARY)                                               */
/******************************************************************************/
#if (RK_CONF_SEMAPHORE == ON)
/**
 * @brief               Create a semaphore from the semaphore object pool.
 * @param semaHandlePtr Pointer to a handle variable. The variable must be
 *                      RK_NULL_HANDLE.
 * @param objName       NUL-terminated object name.
 * @param initValue     Initial value (0 <= initValue <= maxValue)
 * @param maxValue      Maximum value - after reaching this value the
 *                      semaphore does not increment its counter.
 * @return              Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_BUFFER_EMPTY
 *                                   RK_ERR_ERROR
 */
RK_ERR kSemaphoreCreate(RK_HANDLE *const semaHandlePtr,
                        RK_STRING objName,
                        UINT const initValue,
                        UINT const maxValue);
RK_ERR kSemaphoreCreateGlobalScope(RK_HANDLE *const semaHandlePtr,
                                   RK_STRING objName,
                                   UINT const initValue,
                                   UINT const maxValue);
RK_ERR kSemaphoreCreateDomainScope(RK_HANDLE *const semaHandlePtr,
                                   RK_STRING objName,
                                   UINT const initValue,
                                   UINT const maxValue,
                                   RK_DOMAIN *const domainPtr);
RK_ERR kSemaphoreDestroy(RK_HANDLE *const semaHandlePtr);

/**
 * @brief           Wait on a semaphore
 * @param semaHandle Semaphore handle returned by kSemaphoreCreate().
 * @param timeout   Maximum suspension time
 *
 * Binary semaphores remain semaphore objects. They do not spin in user mode.
 *
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_SEMA_BLOCKED
 *                                   RK_ERR_TIMEOUT
 *                                   RK_ERR_INVALID_TIMEOUT
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kSemaphorePend(RK_HANDLE const semaHandle,
                      const RK_TICK timeout);

/**
 * @brief           Signal a semaphore
 * @param semaHandle Semaphore handle returned by kSemaphoreCreate().
 *
 * This is the semaphore wakeup path; it does not perform spinlock operations.
 *
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_SEMA_FULL
 *
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 */
RK_ERR kSemaphorePost(RK_HANDLE const semaHandle);

/**
 * @brief           Retrieve the counter's value of a semaphore
 * @param  semaHandle Semaphore handle returned by kSemaphoreCreate().
 * @param  countPtr Pointer to INT to store the semaphore's counter value.
 *                  A negative value means the number of
 * blocked tasks. A non-negative value is the semaphore's count.
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 *
 */
RK_ERR kSemaphoreQuery(RK_HANDLE const semaHandle,
                       INT *const countPtr);

#endif
/******************************************************************************/
/* MUTEX SEMAPHORE                                                            */
/******************************************************************************/
#if (RK_CONF_MUTEX == ON)
/**
 * @brief             Create a mutex from the mutex object pool.
 * @param mutexHandlePtr Pointer to a handle variable. The variable must be
 *                       RK_NULL_HANDLE.
 * @param objName       NUL-terminated object name.
 * @param protocol    Mutex protocol (RK_PRIO_NONE / RK_PRIO_INHERITANCE).
 * @return            Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_ERROR
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_BUFFER_EMPTY
 */
RK_ERR kMutexCreate(RK_HANDLE *const mutexHandlePtr, RK_STRING objName,
                    UINT protocol);
RK_ERR kMutexCreateGlobalScope(RK_HANDLE *const mutexHandlePtr,
                               RK_STRING objName,
                               UINT protocol);
RK_ERR kMutexCreateDomainScope(RK_HANDLE *const mutexHandlePtr,
                               RK_STRING objName,
                               UINT protocol,
                               RK_DOMAIN *const domainPtr);
RK_ERR kMutexDestroy(RK_HANDLE *const mutexHandlePtr);

/**
 * @brief           Lock a mutex
 * @param mutexHandle Mutex handle returned by kMutexCreate().
 * @param timeout   Maximum suspension time
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_MUTEX_LOCKED
 *                                   RK_ERR_MUTEX_OWNER_FAULTED
 *                                   RK_ERR_TIMEOUT
 *                                   RK_ERR_INVALID_TIMEOUT
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_MUTEX_REC_LOCK
 */
RK_ERR kMutexLock(RK_HANDLE const mutexHandle, RK_TICK const timeout);

/**
 * @brief           Unlock a mutex
 * @param mutexHandle Mutex handle returned by kMutexCreate().
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 *                                   RK_ERR_MUTEX_NOT_LOCKED
 *                                   RK_ERR_MUTEX_NOT_OWNER
 */
RK_ERR kMutexUnlock(RK_HANDLE const mutexHandle);

/**
 * @brief Retrieves the occupancy state of a mutex (locked/unlocked).
 *        This does not report whether a mutex is poisoned by a faulted owner.
 * @param mutexHandle Mutex handle returned by kMutexCreate().
 * @param statePtr Pointer to store the retrieved state
 *                 (0 unlocked, 1 locked)
 * @return Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_BUFFER_FULL
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 */
RK_ERR kMutexQuery(RK_HANDLE const mutexHandle, UINT *const statePtr);

#endif

/******************************************************************************/
/* SLEEP QUEUE                                                                */
/******************************************************************************/
#if (RK_CONF_SLEEP_QUEUE == ON)
/**
 * @brief           Create a sleep queue from the sleep-queue object pool.
 * @param sleepqHandlePtr Pointer to a handle variable. The variable must be
 *                        RK_NULL_HANDLE.
 * @param objName       NUL-terminated object name.
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_BUFFER_EMPTY
 */
RK_ERR kSleepQueueCreate(RK_HANDLE *const sleepqHandlePtr, RK_STRING objName);
RK_ERR kSleepQueueCreateGlobalScope(RK_HANDLE *const sleepqHandlePtr,
                                    RK_STRING objName);
RK_ERR kSleepQueueCreateDomainScope(RK_HANDLE *const sleepqHandlePtr,
                                    RK_STRING objName,
                                    RK_DOMAIN *const domainPtr);
RK_ERR kSleepQueueDestroy(RK_HANDLE *const sleepqHandlePtr);
/**
 * @brief           Puts the running task to sleep on a Sleep Queue.
 * @param sleepqHandle Sleep queue handle returned by kSleepQueueCreate().
 * @param timeout   Suspension time.
 * @return              Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_NOWAIT
 *                                   RK_ERR_TIMEOUT
 *                                   RK_ERR_INVALID_TIMEOUT
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kSleepQueueSleep(RK_HANDLE const sleepqHandle,
                        const RK_TICK timeout);

/**
 * @brief       Wakes tasks sleeping on a Sleep Queue.
 * @param sleepqHandle Sleep queue handle returned by kSleepQueueCreate().
 * @param nTasks    Number of tasks to wake (0 if all)
 * @param uTasksPtr Pointer to store the number
 *                  of unreleased tasks, if any (opt. NULL).
 *                  If called from ISR, execution may be deferred to the
 *                  post-processing system task and uTasksPtr must be NULL.
 * @return      Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_EMPTY_WAITING_QUEUE
 *                                   RK_ERR_NOWAIT

 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_PARAM
 */

RK_ERR kSleepQueueWake(RK_HANDLE const sleepqHandle, UINT nTasks,
                       UINT *uTasksPtr);
#ifndef kSleepQueueFlush
#define kSleepQueueFlush(o) kSleepQueueWake(o, 0, NULL)
#endif

/**
 * @brief       Wakes a single task  (by priority)
 * @param sleepqHandle Sleep queue handle returned by kSleepQueueCreate().
 * @return      Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_EMPTY_WAITING_QUEUE
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 */
RK_ERR kSleepQueueSignal(RK_HANDLE const sleepqHandle);

/**
 * @brief               Wakes a specific task. Task is removed from the
 *                      Sleep Queue and switched to READY.
 * @param sleepqHandle  Sleep queue handle returned by kSleepQueueCreate().
 * @param taskHandle    Handle of the task to be woken.
 * @return      Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_EMPTY_WAITING_QUEUE
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 */
RK_ERR kSleepQueueReady(RK_HANDLE const sleepqHandle,
                        RK_TASK_HANDLE taskHandle);

/**
 * @brief               Moves a READY task to a Sleep Queue.
 *                      Tasks in other states, including the running task,
 *                      cannot be blocked with this API.
 * @param sleepqHandle  Sleep queue handle returned by kSleepQueueCreate().
 * @param handle        Handle of the task.
 * @return RK_ERR       Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_PARAM
 */
RK_ERR kSleepQueueUnready(RK_HANDLE const sleepqHandle,
                          RK_TASK_HANDLE handle);

/**
 * @brief  Retrieves the number of tasks waiting on the queue.
 * @param  sleepqHandle Sleep queue handle returned by kSleepQueueCreate().
 * @param  nTasksPtr Pointer to where store the value
 * @return Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_BUFFER_FULL
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 */
RK_ERR kSleepQueueQuery(RK_HANDLE const sleepqHandle,
                        ULONG *const nTasksPtr);

#endif

/******************************************************************************/
/* TASK SIGNALS                                                               */
/******************************************************************************/
RK_ERR kSignalHandlerSet(RK_SIGNAL const signal,
                         RK_SIGNAL_HANDLER const handler,
                         VOID *const altStackBasePtr,
                         ULONG const altStackBytes);
RK_ERR kSignalSend(RK_TASK_HANDLE const taskHandle, RK_SIGNAL const signal);
RK_ERR kSignalMaskSet(RK_SIGNAL const enabledMask);
RK_ERR kSignalReturn(VOID);

#if (RK_CONF_MESG_QUEUE == ON)
/******************************************************************************/
/* MESSAGE QUEUE                                                              */
/******************************************************************************/
/**
 * @brief               Create a message queue from the queue object pool.
 * @param queueHandlePtr Pointer to a handle variable. The variable must be
 *                       RK_NULL_HANDLE.
 * @param objName       NUL-terminated object name.
 * @param bufPtr        Caller-provided message storage buffer.
 * @param mesgWords     Message size in words (1, 2, 4, 8 or 16).
 * @param nMesg         Max number of messages.
 * @return              Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_INVALID_MSG_SIZE
 *                                   RK_ERR_INVALID_DEPTH
 *                                   RK_ERR_BUFFER_EMPTY
 */
RK_ERR kMesgQueueCreate(RK_HANDLE *const queueHandlePtr,
                        RK_STRING objName,
                        VOID *const bufPtr,
                        ULONG const mesgWords, ULONG const nMesg);
RK_ERR kMesgQueueCreateGlobalScope(RK_HANDLE *const queueHandlePtr,
                                   RK_STRING objName,
                                   VOID *const bufPtr,
                                   ULONG const mesgWords,
                                   ULONG const nMesg);
RK_ERR kMesgQueueCreateDomainScope(RK_HANDLE *const queueHandlePtr,
                                   RK_STRING objName,
                                   VOID *const bufPtr,
                                   ULONG const mesgWords,
                                   ULONG const nMesg,
                                   RK_DOMAIN *const domainPtr);
RK_ERR kMesgQueueDestroy(RK_HANDLE *const queueHandlePtr);
#define kMboxCreate kMesgQueueCreate
#define kMboxDestroy kMesgQueueDestroy
#if (RK_CONF_MESG_QUEUE_SEND_CALLBACK == ON)

/**
 * @brief            Install callback invoked after a successful send.
 *                   Under MPU, this is a boot/configuration operation; calls
 *                   after BOOT are rejected with RK_ERR_INVALID_PHASE.
 * @param queueHandle Queue handle returned by kMesgQueueCreate().
 * @param cbk        Callback pointer executed within a successful send
 *                   - must be short, non-blocking.
 *                   (NULL to remove)
 * @return           Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_PHASE
 */
RK_ERR kMesgQueueInstallSendCbk(RK_HANDLE const queueHandle,
                                VOID (*cbk)(RK_MESG_QUEUE *));

#endif

/**
 * @brief           Receive a message from a queue
 * @param queueHandle Queue handle returned by kMesgQueueCreate().
 * @param recvPtr   Receiving address
 * @param timeout   Suspension time
 *  @return         Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_BUFFER_EMPTY
 *                                   RK_ERR_TIMEOUT
 *                                   RK_ERR_INVALID_TIMEOUT
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kMesgQueueRecv(RK_HANDLE const queueHandle,
                      VOID *const recvPtr,
                      const RK_TICK timeout);

/**
 * @brief           Send a message to a message queue
 * @param queueHandle Queue handle returned by kMesgQueueCreate().
 * @param sendPtr   Message address
 * @param timeout   Suspension time
 *  @return         Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_BUFFER_FULL
 *                                   RK_ERR_TIMEOUT
 *                                   RK_ERR_INVALID_TIMEOUT
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kMesgQueueSend(RK_HANDLE const queueHandle,
                      VOID *const sendPtr,
                      const RK_TICK timeout);


/**
 * @brief           Resets a Message Queue to its initial state.
 *                  Any blocked tasks are released.
 *                  If called from ISR, execution may be deferred to the
 *                  post-processing system task.
 * @param queueHandle Queue handle returned by kMesgQueueCreate().
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 */

RK_ERR kMesgQueueReset(RK_HANDLE const queueHandle);

/**
 * @brief           Receive the front message of a queue
 *                  without changing its state
 * @param queueHandle Queue handle returned by kMesgQueueCreate().
 * @param recvPtr   Receiving pointer
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_BUFFER_EMPTY
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 */
RK_ERR kMesgQueuePeek(RK_HANDLE const queueHandle,
                      VOID *const recvPtr);

/**
 * @brief           Sends a message to the queue front.
 * @param queueHandle Queue handle returned by kMesgQueueCreate().
 * @param sendPtr   Message address
 * @param timeout   Suspension time
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_BUFFER_FULL
 *                                   RK_ERR_TIMEOUT
 *                                   RK_ERR_INVALID_TIMEOUT
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kMesgQueueJam(RK_HANDLE const queueHandle,
                     VOID *const sendPtr,
                     const RK_TICK timeout);

/**
 * @brief           Retrieves message queue counters.
 * @param queueHandle Queue handle returned by kMesgQueueCreate().
 * @param nMesgPtr  Pointer to store the retrieved number (opt NULL).
 * @param nWaitRPtr Pointer to store the number of waiting receivers (opt NULL).
 * @param nWaitSPtr Pointer to store the number of waiting senders (opt NULL).
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_INVALID_PARAM
 */

RK_ERR kMesgQueueQuery(RK_HANDLE const queueHandle,
                       UINT *const nMesgPtr, UINT *const nWaitRPtr,
                       UINT *const nWaitSPtr);
#ifndef kMesgQueueQueryMessageCount
#define kMesgQueueQueryMessageCount(KOBJ, N_MESG_PTR)                          \
    kMesgQueueQuery((KOBJ), (N_MESG_PTR), (NULL), (NULL))
#endif
#ifndef kMesgQueueQueryWaitingReceivers
#define kMesgQueueQueryWaitingReceivers(KOBJ, N_WAIT_R_PTR)                    \
    kMesgQueueQuery((KOBJ), (NULL), (N_WAIT_R_PTR), (NULL))
#endif
#ifndef kMesgQueueQueryWaitingSenders
#define kMesgQueueQueryWaitingSenders(KOBJ, N_WAIT_S_PTR)                      \
    kMesgQueueQuery((KOBJ), (NULL), (NULL), (N_WAIT_S_PTR))
#endif
/**
 * @brief           Overwrites the current message.
 *                  Only valid for single-message queues.
 * @param queueHandle Queue handle returned by kMesgQueueCreate().
 * @param sendPtr   Message address
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_MESGQ_NOT_A_MBOX
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 */
RK_ERR kMesgQueuePostOvw(RK_HANDLE const queueHandle,
                         VOID *sendPtr);

/**
 * @brief           Broadcast a message to currently blocked broadcast
 *                  receivers. Only valid for single-message queues.
 *                  Fails without depositing the message if no broadcast
 *                  receiver is blocked. If more than one receiver is targeted,
 *                  receiver wakeup is deferred to PostProcSysTask.
 * @param queueHandle Queue handle returned by kMesgQueueCreate().
 * @param sendPtr   Message address
 * @param nRecvPtr  Optional pointer receiving the number of tasks targeted.
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_MESGQ_NOT_A_MBOX
 *                                   RK_ERR_BUFFER_FULL
 *                                   RK_ERR_BUFFER_EMPTY
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 */
RK_ERR kMesgQueueBroadcast(RK_HANDLE const queueHandle,
                           VOID *const sendPtr, UINT *const nRecvPtr);
#define kMboxBroadcast kMesgQueueBroadcast /* alias */

/**
 * @brief           Receive a broadcast message from a single-message queue.
 * @param queueHandle Queue handle returned by kMesgQueueCreate().
 * @param recvPtr   Receiving address
 * @param timeout   Suspension time
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_BUFFER_EMPTY
 *                                   RK_ERR_TIMEOUT
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kMesgQueueBroadcastRecv(RK_HANDLE const queueHandle,
                               VOID *const recvPtr,
                               const RK_TICK timeout);
#define kMboxBroadcastRecv kMesgQueueBroadcastRecv /* alias */
/**
 * @brief Declares the appropriate buffer to be used
 *        by a Message Queue.
 * @param BUFNAME Name of the array.
 * @param MESG_TYPE Type of the message.
 * @param N_MESG   Number of messages
 *
 */
#ifndef RK_DECLARE_LOCAL_MESG_QUEUE_BUF
#define RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)            \
    ULONG BUFNAME[RK_MESGQ_BUF_SIZE(MESG_TYPE, N_MESG)] RK_APP_RAM_ATTR;
#endif

#ifndef RK_DECLARE_LOCAL_MESG_QUEUE
#define RK_DECLARE_LOCAL_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)    \
    RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)                \
    RK_DECLARE_LOCAL_MESG_QUEUE_HANDLE(QUEUE_NAME)
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_QUEUE_BUF
#define RK_DECLARE_GLOBAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)           \
    ULONG BUFNAME[RK_MESGQ_BUF_SIZE(MESG_TYPE, N_MESG)] K_ALIGN(4)             \
        RK_SHARED_RAM_ATTR;
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_QUEUE
#define RK_DECLARE_GLOBAL_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)   \
    RK_DECLARE_GLOBAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)               \
    RK_DECLARE_GLOBAL_MESG_QUEUE_HANDLE(QUEUE_NAME)
#endif

#ifndef RK_DECLARE_MESG_QUEUE_BUF
#define RK_DECLARE_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)                  \
    RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, N_MESG)
#endif

#ifndef kMboxQueryMessageCount
#define kMboxQueryMessageCount(KOBJ, N_MESG_PTR)                               \
    kMesgQueueQueryMessageCount((KOBJ), (N_MESG_PTR))
#endif
#ifndef kMboxQueryWaitingReceivers
#define kMboxQueryWaitingReceivers(KOBJ, N_WAIT_R_PTR)                         \
    kMesgQueueQueryWaitingReceivers((KOBJ), (N_WAIT_R_PTR))
#endif
#ifndef kMboxQueryWaitingSenders
#define kMboxQueryWaitingSenders(KOBJ, N_WAIT_S_PTR)                           \
    kMesgQueueQueryWaitingSenders((KOBJ), (N_WAIT_S_PTR))
#endif
#ifndef kMboxQuery
#define kMboxQuery kMesgQueueQuery /* alias */
#endif
#define kMboxPost kMesgQueueSend /* alias */
#define kMboxPend kMesgQueueRecv /* alias */
#define kMboxReset kMesgQueueReset /* alias */

#ifndef RK_DECLARE_MESG_QUEUE
#define RK_DECLARE_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)          \
    RK_DECLARE_LOCAL_MESG_QUEUE(QUEUE_NAME, BUFNAME, MESG_TYPE, N_MESG)
#endif

#ifndef RK_DECLARE_LOCAL_MBOX_BUF
#define RK_DECLARE_LOCAL_MBOX_BUF(BUFNAME, MESG_TYPE)                          \
    RK_DECLARE_LOCAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, 1U)
#endif

#ifndef RK_DECLARE_LOCAL_MBOX
#define RK_DECLARE_LOCAL_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)                   \
    RK_DECLARE_LOCAL_MBOX_BUF(BUFNAME, MESG_TYPE)                              \
    RK_DECLARE_LOCAL_MBOX_HANDLE(MBOX_NAME)
#endif

#ifndef RK_DECLARE_GLOBAL_MBOX_BUF
#define RK_DECLARE_GLOBAL_MBOX_BUF(BUFNAME, MESG_TYPE)                         \
    RK_DECLARE_GLOBAL_MESG_QUEUE_BUF(BUFNAME, MESG_TYPE, 1U)
#endif

#ifndef RK_DECLARE_GLOBAL_MBOX
#define RK_DECLARE_GLOBAL_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)                  \
    RK_DECLARE_GLOBAL_MBOX_BUF(BUFNAME, MESG_TYPE)                             \
    RK_DECLARE_GLOBAL_MBOX_HANDLE(MBOX_NAME)
#endif

#ifndef RK_DECLARE_MBOX_BUF
#define RK_DECLARE_MBOX_BUF(BUFNAME, MESG_TYPE)                                \
    RK_DECLARE_LOCAL_MBOX_BUF(BUFNAME, MESG_TYPE)
#endif

#ifndef RK_DECLARE_MBOX
#define RK_DECLARE_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)                         \
    RK_DECLARE_LOCAL_MBOX(MBOX_NAME, BUFNAME, MESG_TYPE)
#endif


#endif /* RK_CONF_MESG_QUEUE */

/******************************************************************************/
/* ASYNCHRONOUS TASK MESSAGES                                                 */
/******************************************************************************/
#if ((RK_CONF_ASYNCH_MESG == ON) && (RK_CONF_MESG_QUEUE == ON))
/**
 * The by-reference asynchronous direct path provides task-to-task message
 * passing with RK_MESG pointers. Messages are fixed-size blocks allocated from
 * caller-provided memory partition storage. kMesgAlloc() can wait for pool
 * availability. kMesgSend() transfers ownership of an allocated message to a
 * task endpoint; on success, the sender must not touch the message again. The
 * receiver obtains the message pointer with kMesgWait() and returns it to the
 * originating pool with kMesgFree().
 *
 * With the MPU enabled, by-reference direct async messages are same-domain
 * only. Use kMesgQueueSend() / kMesgQueueRecv() when a queue is the shared
 * rendezvous point and payloads should be copied. Use
 * kMesgSendCopy() / kMesgRecvCopy() when the receiver task itself is the
 * endpoint: the kernel allocates an internal RK_MESG block, copies the payload,
 * accounts for delivery, and frees the internal block after receive or cleanup.
 * Priority ceiling, when configured on the pool, is part of this asynchronous
 * ownership contract and is separate from mutex priority inheritance and
 * synchronous call/reply priority substitution.
 */
/**
 * @brief Initialise a task-backed async direct-message endpoint.
 * @param taskHandle Task that will receive messages with kMesgWait().
 * @return           Successful:
 *                                   RK_ERR_SUCCESS
 *                   Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_OBJ_DOUBLE_INIT
 *                                   RK_ERR_HAS_OWNER
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kMesgEndpointInit(RK_TASK_HANDLE const taskHandle);

/**
 * @brief Initialise a pool for fixed-size direct messages.
 *
 *        Pass RK_MESG_PRIO_CEILING_NONE when the pool does not need priority
 *        ceiling. Otherwise, while a task owns at least one message from this
 *        pool, it runs no lower than ceilingPrio until ownership is transferred
 *        or the message is freed.
 *
 * @param poolPtr       Memory partition object used as the message pool.
 * @param memPoolPtr    Aligned backing storage.
 * @param payloadBytes  Payload bytes available after the RK_MESG header.
 * @param nMesg         Number of message blocks in the pool.
 * @param ceilingPrio   Highest priority required while owning pool messages,
 *                      or RK_MESG_PRIO_CEILING_NONE. Lower numeric RK_PRIO
 *                      values represent higher scheduler priorities.
 * @return              RK_ERR_SUCCESS, RK_ERR_INVALID_PARAM, or
 *                      RK_ERR_INVALID_PRIO.
 */
RK_ERR kMesgPoolInit(RK_MEM_PARTITION *const poolPtr,
                     VOID *const memPoolPtr,
                     ULONG const payloadBytes,
                     ULONG const nMesg,
                     RK_PRIO const ceilingPrio);
RK_ERR kMesgPoolInitGlobalScope(RK_MEM_PARTITION *const poolPtr,
                                VOID *const memPoolPtr,
                                ULONG const payloadBytes,
                                ULONG const nMesg,
                                RK_PRIO const ceilingPrio);
RK_ERR kMesgPoolInitDomainScope(RK_MEM_PARTITION *const poolPtr,
                                VOID *const memPoolPtr,
                                ULONG const payloadBytes,
                                ULONG const nMesg,
                                RK_PRIO const ceilingPrio,
                                RK_DOMAIN *const domainPtr);

/**
 * @brief Allocate one message from a direct-message pool.
 * @param poolPtr      Message pool initialised with kMesgPoolInit().
 * @param mesgPPtr   Receives an allocated message pointer on success.
 * @param timeout      RK_NO_WAIT, RK_WAIT_FOREVER, or bounded ticks.
 * @return             Successful:
 *                                   RK_ERR_SUCCESS
 *                     Unsuccessful:
 *                                   RK_ERR_BUFFER_EMPTY
 *                                   RK_ERR_TIMEOUT
 *                     Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_INVALID_TIMEOUT
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kMesgAlloc(RK_MEM_PARTITION *const poolPtr,
                  RK_MESG **const mesgPPtr,
                  RK_TICK const timeout);

/**
 * @brief Return an allocated or received message to its originating pool.
 */
RK_ERR kMesgFree(RK_MESG *const mesgPtr);

/**
 * @brief Return the application payload address carried by a message.
 */
VOID *kMesgPayload(RK_MESG *const mesgPtr);
VOID const *kMesgPayloadConst(RK_MESG const *const mesgPtr);

/**
 * @brief Return payload capacity in bytes for this message block.
 */
ULONG kMesgPayloadBytes(RK_MESG const *const mesgPtr);

/**
 * @brief Return the sender task handle recorded at kMesgSend().
 */
RK_TASK_HANDLE kMesgGetSenderHandle(RK_MESG const *const mesgPtr);

/**
 * @brief Return the sender task ID recorded at kMesgSend().
 * @param mesgPtr      Message with a recorded sender.
 * @param senderIDPtr  Receives the sender task ID.
 * @return             RK_ERR_SUCCESS, RK_ERR_OBJ_NULL, RK_ERR_INVALID_OBJ, or
 *                     RK_ERR_MESG_INVALID_STATE when no sender has been
 *                     recorded.
 */
RK_ERR kMesgGetSenderID(RK_MESG const *const mesgPtr,
                        RK_TID *const senderIDPtr);

/**
 * @brief Transfer a message to a task endpoint.
 *        On success, the sender must not touch the message again. With MPU,
 *        both tasks must be able to access the message pool memory.
 * @param taskHandle Destination task with an async endpoint.
 * @param mesgPtr    Message allocated by kMesgAlloc().
 * @return           Successful:
 *                                   RK_ERR_SUCCESS
 *                   Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_MESG_INVALID_STATE
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kMesgSend(RK_TASK_HANDLE const taskHandle,
                 RK_MESG *const mesgPtr);

/**
 * @brief Wait for one async direct message sent to the running task.
 * @param fromTaskHandle RK_ANY_TASK or a specific sender task handle.
 * @param mesgPPtr     Receives the message pointer on success.
 * @param timeout        RK_NO_WAIT, RK_WAIT_FOREVER, or bounded ticks.
 * @return               Successful:
 *                                   RK_ERR_SUCCESS
 *                       Unsuccessful:
 *                                   RK_ERR_BUFFER_EMPTY
 *                                   RK_ERR_TIMEOUT
 *                       Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_INVALID_TIMEOUT
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kMesgWait(RK_TASK_HANDLE const fromTaskHandle,
                 RK_MESG **const mesgPPtr,
                 RK_TICK const timeout);

#if (RK_CONF_ASYNCH_COPY_MESG == ON)
/**
 * @brief Initialise a task-backed async copy-message endpoint.
 *
 *        A task may own one message endpoint model: synchronous direct,
 *        asynchronous pointer-direct, or asynchronous copy. Copy endpoints are
 *        intended for cross-domain traffic because the kernel copies payloads
 *        into a bounded internal pool and never returns RK_MESG pointers to the
 *        application. The kernel owns and frees those internal message blocks.
 */
RK_ERR kMesgCopyEndpointInit(RK_TASK_HANDLE const taskHandle);

/**
 * @brief Send one asynchronous copy message to a task endpoint.
 *
 *        The payload is copied before the call returns. If the receiver is
 *        blocked in kMesgRecvCopy() and the payload fits the advertised receive
 *        buffer, the receiver is woken directly. If the payload does not fit,
 *        the receiver is left blocked and the message is queued for a later
 *        receive with enough space.
 */
RK_ERR kMesgSendCopy(RK_TASK_HANDLE const taskHandle,
                     VOID const *const sendPtr,
                     ULONG const bytes);

/**
 * @brief Receive one asynchronous copy message sent to the running task.
 *
 *        Only messages from fromTaskHandle are accepted, or RK_ANY_TASK accepts
 *        any sender. The receive completes only for a message whose payload fits
 *        recvBytes; larger queued messages remain pending.
 */
RK_ERR kMesgRecvCopy(RK_TASK_HANDLE const fromTaskHandle,
                     VOID *const recvPtr,
                     ULONG const recvBytes,
                     ULONG *const rxBytesPtr,
                     RK_TICK const timeout);
#endif

#ifndef RK_MESG_BLOCK_SIZE_BYTES
#define RK_MESG_BLOCK_SIZE_BYTES(MESG_TYPE)                                   \
    ((ULONG)((sizeof(RK_MESG) + sizeof(MESG_TYPE) + RK_WORD_SIZE - 1UL) &     \
             ~(RK_WORD_SIZE - 1UL)))
#endif

#ifndef RK_MESG_POOL_WORDS
#define RK_MESG_POOL_WORDS(MESG_TYPE, N_MESG)                                 \
    ((UINT)((RK_MESG_BLOCK_SIZE_BYTES(MESG_TYPE) / RK_WORD_SIZE) * (N_MESG)))
#endif

#ifndef RK_DECLARE_MESG_POOL_BUF
#define RK_DECLARE_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)                  \
    RK_DECLARE_LOCAL_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)
#endif

#ifndef RK_DECLARE_LOCAL_MESG_POOL_BUF
#define RK_DECLARE_LOCAL_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)            \
    ULONG BUFNAME[RK_MESG_POOL_WORDS(MESG_TYPE, N_MESG)] RK_APP_RAM_ATTR;
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_POOL_BUF
#define RK_DECLARE_GLOBAL_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)           \
    ULONG BUFNAME[RK_MESG_POOL_WORDS(MESG_TYPE, N_MESG)] K_ALIGN(4)           \
        RK_SHARED_RAM_ATTR;
#endif

#ifndef RK_DECLARE_MESG_POOL
#define RK_DECLARE_MESG_POOL(POOL_NAME, BUFNAME, MESG_TYPE, N_MESG)           \
    RK_DECLARE_LOCAL_MESG_POOL(POOL_NAME, BUFNAME, MESG_TYPE, N_MESG)
#endif

#ifndef RK_DECLARE_LOCAL_MESG_POOL
#define RK_DECLARE_LOCAL_MESG_POOL(POOL_NAME, BUFNAME, MESG_TYPE, N_MESG)     \
    RK_DECLARE_LOCAL_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)                \
    RK_DECLARE_LOCAL_MEM_PARTITION(POOL_NAME)
#endif

#ifndef RK_DECLARE_GLOBAL_MESG_POOL
#define RK_DECLARE_GLOBAL_MESG_POOL(POOL_NAME, BUFNAME, MESG_TYPE, N_MESG)    \
    RK_DECLARE_GLOBAL_MESG_POOL_BUF(BUFNAME, MESG_TYPE, N_MESG)               \
    RK_DECLARE_GLOBAL_MEM_PARTITION(POOL_NAME)
#endif

#ifndef RK_MESG_PAYLOAD
#define RK_MESG_PAYLOAD(MESG_PTR, MESG_TYPE)                                  \
    ((MESG_TYPE *)kMesgPayload((MESG_PTR)))
#endif

#endif /* RK_CONF_ASYNCH_MESG && RK_CONF_MESG_QUEUE */
/**
 * @note
 * A task may be initialised to handle either Direct Synchronous Message or
 * Asynchronous Direct Message, but not both.
 */

/******************************************************************************/
/* SYNCHRONOUS MESSAGE (UNBUFFERED MESSAGE PASSING)                           */
/******************************************************************************/
#if (RK_CONF_SYNCH_MESG == ON)
/**
 * Synchronous Message provides two direct-copy contracts over a task endpoint.
 * kSynchSendWait()/kSyncRecv() is a plain blocking rendezvous: the sender gives
 * one non-NULL source buffer plus the actual byte count and remains blocked
 * only until the receiver copies that payload into receiver-owned storage.
 * kSynchMesgCall()/kSynchMesgAccept()/kSynchMesgReply() is an extended
 * rendezvous: the caller remains blocked until the server replies.
 *
 * This is a direct-message contract, not a shared-memory lock. It may be used
 * with private or shared memory as long as the syscall boundary can validate
 * the source and destination ranges. Only the extended call/reply form carries
 * priority: a server with queued or active callers runs at caller effective
 * priority until reply, timeout, or cleanup. This is priority substitution, not
 * privilege delegation.
 *
 * A task that owns any mutex must not send or receive through Synchronous
 * Message; those operations return RK_ERR_TASK_INVALID_ST.
 */
/**
 * @brief Initialise the task-backed Synchronous Message endpoint.
 * @param taskHandle Task that owns the single Synchronous Message receive slot.
 * @param maxMesgBytes Maximum message size, in bytes, accepted by this
 *                     endpoint. Must be non-zero and a multiple of
 *                     RK_WORD_SIZE.
 * @return           Successful:
 *                                   RK_ERR_SUCCESS
 *                   Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_HAS_OWNER
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kSynchMesgInit(RK_TASK_HANDLE const taskHandle,
                      ULONG const maxMesgBytes);

/**
 * @brief Send a payload directly to a task and block until copied.
 *        Success means the receiver has copied the payload before the sender
 *        was released; it does not mean the receiver has processed it or
 *        produced an answer. This plain rendezvous does not substitute receiver
 *        priority.
 *        A bounded timeout covers both waiting for the receive slot and waiting
 *        for the receiver to copy the message.
 * @param taskHandle Receiver task handle.
 * @param mesgPtr    Non-NULL source buffer.
 * @param mesgBytes  Actual message size, in bytes. Must be non-zero, a multiple
 *                   of RK_WORD_SIZE, and no larger than the receiver endpoint
 *                   maximum configured in kSynchMesgInit().
 * @param timeout    Suspension time.
 * @return           Successful:
 *                                   RK_ERR_SUCCESS
 *                   Unsuccessful:
 *                                   RK_ERR_NOWAIT
 *                                   RK_ERR_TIMEOUT
 *                                   RK_ERR_INVALID_TIMEOUT
 *                                   RK_ERR_TASK_INVALID_ST
 *                                   RK_ERR_INVALID_MSG_SIZE
 *                   Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kSynchSendWait(RK_TASK_HANDLE const taskHandle,
                      VOID const *const mesgPtr,
                      ULONG const mesgBytes,
                      RK_TICK const timeout);

#ifndef kSynchMesgSend
#define kSynchMesgSend(TASK_HANDLE, MESG_PTR, MESG_BYTES, TIMEOUT)                 \
    kSynchSendWait((TASK_HANDLE), (MESG_PTR), (MESG_BYTES), (TIMEOUT))
#endif

/**
 * @brief Receive the payload sent to the running task.
 *        On success, the payload is copied into recvPtr before the blocked
 *        sender is released. recvPtr must point to storage large enough for
 *        the maximum message size configured in kSynchMesgInit().
 * @param recvPtr      Non-NULL destination buffer.
 * @param mesgBytesPtr Optional pointer receiving the actual copied byte count.
 * @param timeout      Suspension time.
 * @return             Successful:
 *                                   RK_ERR_SUCCESS
 *                     Unsuccessful:
 *                                   RK_ERR_BUFFER_EMPTY
 *                                   RK_ERR_TIMEOUT
 *                                   RK_ERR_INVALID_TIMEOUT
 *                                   RK_ERR_TASK_INVALID_ST
 *                     Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kSyncRecv(VOID *const recvPtr,
                 ULONG *const mesgBytesPtr,
                 RK_TICK const timeout);

#ifndef kSynchMesgRecv
#define kSynchMesgRecv(RECV_PTR, MESG_BYTES_PTR, TIMEOUT)                          \
    kSyncRecv((RECV_PTR), (MESG_BYTES_PTR), (TIMEOUT))
#endif

/**
 * @brief Invoke a server task and wait for its reply.
 *        The request is copied into server storage by kSynchMesgAccept().
 *        The caller remains blocked until kSynchMesgReply() copies a reply
 *        back, or until the timeout expires. This extended rendezvous carries
 *        caller effective priority to the server.
 * @param taskHandle Server task handle.
 * @param attrPtr    Non-NULL invocation attributes. reqPtr/replyPtr must be
 *                   non-NULL. reqBytes is the request size. replyMaxBytes is
 *                   the caller reply-buffer capacity. replyBytesPtr optionally
 *                   receives the actual reply byte count.
 * @param timeout    RK_WAIT_FOREVER or bounded ticks. RK_NO_WAIT invalid.
 */
RK_ERR kSynchMesgCall(RK_TASK_HANDLE const taskHandle,
                      RK_SYNCH_ATTR const *const attrPtr,
                      RK_TICK const timeout);

#ifndef kSynchMesgInvoke
#define kSynchMesgInvoke(TASK_HANDLE, ATTR_PTR, TIMEOUT)                      \
    kSynchMesgCall((TASK_HANDLE), (ATTR_PTR), (TIMEOUT))
#endif

/**
 * @brief Accept one pending invocation on the running task.
 *        On success, the request is copied into recvPtr, callPtr is filled
 *        with server-local extended-rendezvous metadata, caller priority is
 *        latched for the server, and the caller remains blocked until
 *        kSynchMesgReply().
 */
RK_ERR kSynchMesgAccept(RK_SYNCH_CALL_DATA *const callPtr,
                        VOID *const recvPtr,
                        ULONG *const reqBytesPtr,
                        RK_TICK const timeout);

/**
 * @brief Reply to a previously accepted invocation.
 *        If the caller timed out after accept, this completes the abandoned
 *        extended rendezvous and no reply is copied.
 */
RK_ERR kSynchMesgReply(RK_SYNCH_CALL_DATA const *const callPtr,
                       VOID const *const replyPtr,
                       ULONG const replyBytes);

#endif /* RK_CONF_SYNCH_MESG */

/******************************************************************************/
/* SYSTEM MONITOR TERMINAL                                                     */
/******************************************************************************/
#if (RK_CONF_SYSMON == ON)
/**
 * @brief Start the lightweight console-backed system monitor task.
 *
 *        SysMon uses the privileged console driver service and prints
 *        on-demand snapshots of tasks and registered kernel objects. If another
 *        console front-end already owns foreground RX, SysMon starts without
 *        consuming terminal input. It is
 *        deliberately smaller than the optional trace recorder: objects are
 *        held in one intrusive linked list per object family, and commands
 *        print bounded point-in-time state rather than histories.
 *
 *        The first terminal input enters a diagnosis session. During that
 *        session, normal kLog() console output is muted so command output is
 *        not interleaved with application logs. Fault-level output remains
 *        live. Use `exit` or `quit` to restore the previous logger state.
 *
 *        The `rk>` prompt accepts:
 *
 *        help          Command summary.
 *        list tasks    Task status, priority, events and stack watermark.
 *        list objects  Count of registered objects by family.
 *        list sema     Semaphore values and waiters.
 *        list mutex    Mutex lock/owner/waiter state.
 *        list queue    Message queue occupancy and waiters.
 *        list timers   Application timer state.
 *        list mem      Memory partition use.
 *        list sleepq   Sleep queue waiters.
 *        list mrm      Most-recent-message state.
 *        list shared   Shared-memory attachments.
 *        exit          Leave diagnosis mode and restore normal logger output.
 *
 * @return RK_ERR_SUCCESS on success. If SysMon was already started, the call
 *         is idempotent and returns RK_ERR_SUCCESS. Otherwise returns the
 *         console-service or kTaskInitPrivileged() error for the monitor task.
 */
RK_ERR kSysMonInit(VOID);

/**
 * @brief Drain SysMon's console-service input queue and execute commands.
 *
 *        kSysMonInit() creates a privileged task that calls this function.
 *        It remains public for trusted applications that want to call the
 *        monitor from their own privileged service loop.
 */
VOID kSysMonPoll(VOID);

/**
 * @brief Submit one complete command line to the SysMon task.
 *
 *        This is for applications that own foreground console RX and route
 *        diagnostic input explicitly, for example after an `RKMONITOR` prefix.
 *        The submitted line excludes the prefix and CR/LF terminator. When
 *        called from an unprivileged task, the line range is validated at the
 *        syscall boundary and copied into SysMon's private input queue. The
 *        privileged SysMon task later executes the command and prints through
 *        the console service.
 *
 * @param linePtr   Command text without CR/LF.
 * @param lineBytes Number of command bytes, at most
 *                  RK_CONF_SYSMON_LINE_LEN - 1.
 * @return RK_ERR_SUCCESS, RK_ERR_OBJ_NULL, RK_ERR_OBJ_NOT_INIT,
 *         RK_ERR_INVALID_PARAM, RK_ERR_INVALID_ISR_PRIMITIVE, or
 *         RK_ERR_BUFFER_FULL.
 */
RK_ERR kSysMonCommand(CHAR const *linePtr, ULONG lineBytes);

/**
 * @brief Attach a short display name to a registered kernel object.
 *
 *        Create APIs take the initial object name. This helper remains for
 *        BOOT-created static objects and intentional renames. The handle may be
 *        an encoded runtime object handle or a raw pointer to a BOOT-created
 *        object. Names are copied into the common object header and truncated
 *        to RK_NAME_SIZE, including the trailing NUL. Use the kObjectNameSet()
 *        helper form:
 *
 *        kObjectNameSet(queueHandle, "FleetQ");
 *
 * @param objHandle Kernel object handle or raw object pointer.
 * @param namePtr NUL-terminated object name.
 * @return RK_ERR_SUCCESS on success, RK_ERR_OBJ_NULL for NULL parameters, or
 *         RK_ERR_INVALID_OBJ if the object is not registered for SysMon.
 */
RK_ERR kSysMonObjectNameSet(RK_HANDLE const objHandle,
                            CHAR const *const namePtr);

#ifndef kObjectNameSet
#define kObjectNameSet(OBJ_HANDLE, NAME_PTR)                                  \
    kSysMonObjectNameSet((RK_HANDLE)(UINTPTR)(OBJ_HANDLE), (NAME_PTR))
#endif
#endif /* RK_CONF_SYSMON */

/******************************************************************************/
/* KERNEL TRACE CONSOLE                                                       */
/******************************************************************************/
#if (RK_CONF_TRACE == ON)
/**
 * @brief Start the UART-backed kernel trace console task.
 *
 *        The console reads characters with kTraceUartGetc(). The platform UART
 *        backend should enable RX interrupts with kTraceUartRxEnable(), buffer
 *        received characters in the UART ISR, then call
 *        kTraceInputSignalFromISR() so the trace task wakes by task event.
 *        Applications must provide those UART hooks when the weak defaults are
 *        not sufficient.
 *        The console prompt accepts:
 *
 *        top           Task run count, CPU/window, priority, stack watermark,
 *                      and events.
 *        list kobjects Registered trace objects and last recorded operation.
 *        list kmesg    Registered message queues.
 *        list kipc     Task-backed Synchronous/Invocation and Asynchronous
 *                      Direct Message endpoint and wait state; SVEF/SVNOM show
 *                      server/receiver effective/nominal priority.
 *        list ksema    Registered semaphores and mutexes.
 *        list kmem     Registered memory partitions.
 *        list ksleepq  Sleep queue state.
 *        list kmrm     Most-recent-message state.
 *        list ktimers  Application timer state.
 *        list ktimerq  Raw application timer delta list.
 *        hist [name]   Operation history for one named object, or all objects.
 *        hist task/X   Priority-change history for task name or TID X.
 *        history ...   Alias for hist.
 *        dump [frames] Flush buffered KTRACE_FRAME records to trace output.
 *        help          Command summary.
 *
 *        When a trace history ring overwrites an old record, the evicted record
 *        is queued to the trace task and passed to kTraceOverflowPersist().
 *        By default that hook buffers `KTRACE_FRAME` hex records so the trace
 *        console is not flooded; the `dump` command flushes those records to
 *        the trace output. Targets can set RK_CONF_TRACE_FRAME_STDOUT to ON for
 *        immediate printing, or override the weak hook with a board-specific
 *        flash append routine.
 *
 * @return RK_ERR_SUCCESS on success. If trace was already started, the call is
 *         idempotent and also returns RK_ERR_SUCCESS. Otherwise returns the
 *         kTaskInit() error for the trace console task.
 */
RK_ERR kTraceInit(VOID);

/**
 * @brief Poll the trace UART input and execute complete console commands.
 *
 *        kTraceInit() creates a task that calls this function when UART RX
 *        signals its task event. kTracePoll() remains public for applications
 *        that want to drain trace input from their own service loop.
 */
VOID kTracePoll(VOID);

/**
 * @brief Wake the trace console task after UART RX input is buffered.
 *
 *        This function is ISR-safe when a trace task exists because it signals
 *        that task explicitly with a task event. It is a no-op before
 *        kTraceInit() creates the trace task.
 */
VOID kTraceInputSignalFromISR(VOID);

/**
 * @brief Attach a short user name to a registered kernel object.
 *
 *        Create APIs take the initial object name. This helper remains for
 *        BOOT-created static objects and intentional renames. The name is
 *        stored in the object's objName field and truncated to fit
 *        RK_NAME_SIZE, including the trailing NUL. Use kTraceNameObject() as
 *        the public convenience macro:
 *
 *        kTraceNameObject(queueHandle, "UartQ");
 *
 * @param objHandle Traceable object handle.
 * @param namePtr NUL-terminated name string.
 * @return RK_ERR_SUCCESS on success, RK_ERR_OBJ_NULL for NULL parameters, or
 *         RK_ERR_INVALID_OBJ if the object is not traceable/registered.
 */
RK_ERR kTraceObjectNameSet(RK_HANDLE const objHandle,
                           CHAR const *const namePtr);

/**
 * @brief Record one operation in an object's circular trace history.
 *
 *        This is mainly used by kernel object implementations. Application code
 *        normally only names objects and reads snapshots/history. The record
 *        stores the current tick, running task TID, operation, return code, and
 *        one operation-specific numeric value. Trace operations ending in `_BLOCK`
 *        identify the operation that suspended the running task.
 *
 * @param objPtr Pointer to the registered object.
 * @param op     Operation code.
 * @param result Return/error code associated with the operation.
 * @param value  Operation-specific value, such as queue depth or timer delay.
 */
VOID kTraceRecordObject(VOID *const objPtr, RK_TRACE_OP const op,
                        RK_ERR const result, ULONG const value);

/**
 * @brief Record one effective-priority change for a task.
 *
 *        Kernel priority-inheritance/adoption code calls this after changing
 *        a task's effective priority. The counter is surfaced by the trace `top`
 *        command, and the detailed circular history is surfaced by
 *        `hist task/<name>` or `hist task/<tid>`.
 *
 * @param taskHandle  Task whose effective priority changed.
 * @param oldPriority Previous effective priority.
 * @param newPriority New effective priority.
 */
VOID kTraceRecordTaskPrio(RK_TASK_HANDLE const taskHandle,
                          RK_PRIO const oldPriority,
                          RK_PRIO const newPriority);

/**
 * @brief Record one periodic-task overrun trace event.
 *
 *        Release overruns come from kSleepRelease() when one or more release
 *        slots were missed. Until overruns come from kSleepUntil() when the
 *        anchored release time has already elapsed. The default trace hook
 *        emits these records as buffered `KTRACE_FRAME` events so the timeline
 *        report can show when each task overran and whether it came from
 *        Release or Until.
 *
 * @param kind    RK_TRACE_OVERRUN_RELEASE or RK_TRACE_OVERRUN_UNTIL.
 * @param period  Period requested by the task.
 * @param lateBy  Ticks late at the overrun point.
 * @param skipped Release slots skipped; zero for Until.
 */
VOID kTraceRecordTaskOverrun(RK_TRACE_OVERRUN_KIND const kind,
                             RK_TICK const period, RK_TICK const lateBy,
                             ULONG const skipped);

/**
 * @brief Persist one deferred trace event from trace task context.
 *
 *        This weak hook is called after a trace record has been copied into the
 *        trace backlog. It does not run in the trace hot path. By default it
 *        buffers hex-encoded binary `KTRACE_FRAME` records; the trace `dump`
 *        command prints them so overflow and task-overrun history can be
 *        captured without flooding the console during normal use.
 *        Set RK_CONF_TRACE_FRAME_STDOUT to ON for immediate printing, or
 *        override this function to use a board-specific flash append routine.
 *
 * @param infoPtr Deferred object, task-priority, or task-overrun record.
 */
VOID kTraceOverflowPersist(RK_TRACE_OVERFLOW_INFO const *const infoPtr);

/**
 * @brief Copy the current task trace snapshot into a user buffer.
 *
 *        eventCurr is the task's current event register. eventReq/eventOpt
 *        describe the currently wanted event mask and ANY/ALL mode only while
 *        the task status is RK_SLEEPING_EV_FLAG; otherwise eventReq is 0 and
 *        eventOpt is 0. stackFirstPtr/stackLastPtr bound the task stack buffer.
 *        stackLowWaterPtr is derived from the stack paint pattern and points to
 *        the lowest stack word observed as used.
 *
 * @param infoPtr Destination array.
 * @param maxInfo Number of entries available in infoPtr.
 * @return Number of entries written.
 */
UINT kTraceTaskSnapshot(RK_TRACE_TASK_INFO *const infoPtr, UINT const maxInfo);

/**
 * @brief Copy message-passing object state into a user buffer.
 *
 *        Includes registered message queues when enabled.
 *
 * @param infoPtr Destination array.
 * @param maxInfo Number of entries available in infoPtr.
 * @return Number of entries written.
 */
#if (RK_CONF_MESG_QUEUE == ON)
UINT kTraceMesgSnapshot(RK_TRACE_OBJECT_INFO *const infoPtr,
                        UINT const maxInfo);
#endif

/**
 * @brief Copy semaphore and mutex state into a user buffer.
 *
 * @param infoPtr Destination array.
 * @param maxInfo Number of entries available in infoPtr.
 * @return Number of entries written.
 */
#if ((RK_CONF_SEMAPHORE == ON) || (RK_CONF_MUTEX == ON))
UINT kTraceSemaSnapshot(RK_TRACE_SYNC_INFO *const infoPtr, UINT const maxInfo);
#endif

/**
 * @brief Copy application timer state into a user buffer.
 *
 *        remainingTicks is the remaining delta-list time from now, including
 *        any initial phase still pending. phase reports the configured initial
 *        phase value.
 *
 * @param infoPtr Destination array.
 * @param maxInfo Number of entries available in infoPtr.
 * @return Number of entries written.
 */
#if (RK_CONF_CALLOUT_TIMER == ON)
UINT kTraceTimerSnapshot(RK_TRACE_TIMER_INFO *const infoPtr,
                         UINT const maxInfo);
#endif

/**
 * @brief Copy an object's operation history into a user buffer.
 *
 *        The newest records are returned first. The maximum available depth is
 *        RK_CONF_TRACE_RECORD_DEPTH.
 *
 * @param objHandle Registered object handle.
 * @param infoPtr Destination array.
 * @param maxInfo Number of entries available in infoPtr.
 * @return Number of entries written.
 */
UINT kTraceRecordSnapshot(RK_HANDLE const objHandle,
                          RK_TRACE_RECORD_INFO *const infoPtr,
                          UINT const maxInfo);

/**
 * @brief Copy a task's effective-priority change history into a user buffer.
 *
 *        Records are returned oldest first. The maximum available depth is
 *        RK_CONF_TRACE_RECORD_DEPTH.
 *
 * @param taskHandle Target task.
 * @param infoPtr    Destination array.
 * @param maxInfo    Number of entries available in infoPtr.
 * @return Number of entries written.
 */
UINT kTraceTaskPrioSnapshot(RK_TASK_HANDLE const taskHandle,
                            RK_TRACE_PRIO_RECORD_INFO *const infoPtr,
                            UINT const maxInfo);

/**
 * @brief Public name helper for kTraceObjectNameSet().
 *
 * @param OBJ_HANDLE Traceable object handle.
 * @param NAME_PTR NUL-terminated object name.
 * @return See kTraceObjectNameSet().
 */
#ifndef kTraceNameObject
#define kTraceNameObject(OBJ_HANDLE, NAME_PTR)                                 \
    kTraceObjectNameSet((RK_HANDLE)(UINTPTR)(OBJ_HANDLE), (NAME_PTR))
#endif
#endif /* RK_CONF_TRACE */

/******************************************************************************/
/* MOST-RECENT MESSAGE PROTOCOL                                               */
/******************************************************************************/
#if (RK_CONF_MRM == ON)
/**
 * MRM publishes the latest value from a bounded buffer set. Payloads are copied
 * at publish/get time, but kMRMReserve() and kMRMGet() return RK_MRM_BUF lease
 * pointers that must later be passed to kMRMPublish() or kMRMUnget().
 *
 * For that reason, MRM is a domain-local service under MPU. A task may operate
 * on an MRM only from the domain that owns it. Plain kMRMCreate() follows the
 * default ownership rule: during BOOT it creates an App-owned MRM, and at
 * runtime it uses the calling task's domain. Use kMRMCreateDomainScope() during
 * BOOT when trusted construction code is intentionally creating an MRM for an
 * explicit domain. Use message queues or task-addressed copy messages for
 * cross-domain latest-value transfer, then keep the latest value locally in
 * the receiving domain.
 */
/**
 * @brief               Create an MRM control block from the MRM object pool.
 * @param mrmHandlePtr  Pointer to a handle variable. The variable must be
 *                      RK_NULL_HANDLE.
 * @param objName       NUL-terminated object name.
 * @param mrmPoolPtr    Caller-provided pool of MRM buffers.
 * @param mesgPoolPtr   Caller-provided pool of message buffers.
 * @param nBufs         Number of MRM buffers; also the number of messages.
 * @param dataSizeWords Size of a message within an MRM, in words.
 * @return              Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_BUFFER_EMPTY
 */
RK_ERR kMRMCreate(RK_HANDLE *const mrmHandlePtr,
                  RK_STRING objName,
                  RK_MRM_BUF *const mrmPoolPtr,
                  VOID *mesgPoolPtr, ULONG const nBufs,
                  ULONG const dataSizeWords);
RK_ERR kMRMCreateDomainScope(RK_HANDLE *const mrmHandlePtr,
                             RK_STRING objName,
                             RK_MRM_BUF *const mrmPoolPtr,
                             VOID *mesgPoolPtr, ULONG const nBufs,
                             ULONG const dataSizeWords,
                             RK_DOMAIN *const domainPtr);
RK_ERR kMRMDestroy(RK_HANDLE *const mrmHandlePtr);

/**
 * @brief       Reserve an MRM buffer to be written.
 * @param mrmHandle MRM handle returned by kMRMCreate().
 * @return      Pointer to an MRM buffer lease, or NULL.
 */
RK_MRM_BUF *kMRMReserve(RK_HANDLE const mrmHandle);

/**
 * @brief           Copy a message into an MRM and make it the most recent
 *                  message.
 * @param mrmHandle MRM handle returned by kMRMCreate().
 * @param bufPtr    Pointer to an MRM buffer lease.
 * @param dataPtr   Pointer to the message to be published.
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_MEM_FREE
 *                                   RK_ERR_INVALID_PARAM
 */
RK_ERR kMRMPublish(RK_HANDLE const mrmHandle, RK_MRM_BUF *const bufPtr,
                   VOID const *dataPtr);

/**
 * @brief           Receive the most recent published message within an MRM.
 * @param mrmHandle MRM handle returned by kMRMCreate().
 * @param getMesgPtr   Pointer to where the message will be copied.
 * @return          Pointer to an MRM buffer lease to be passed to kMRMUnget(),
 *                  or NULL.
 */
RK_MRM_BUF *kMRMGet(RK_HANDLE const mrmHandle, VOID *const getMesgPtr);

/**
 * @brief           Release an MRM buffer lease after its copied message has
 *                  been consumed.
 * @param mrmHandle MRM handle returned by kMRMCreate().
 * @param bufPtr    Pointer to the MRM buffer returned by kMRMGet().
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_NOT_INIT
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_MEM_FREE
 *                                   RK_ERR_INVALID_PARAM
 */
RK_ERR kMRMUnget(RK_HANDLE const mrmHandle, RK_MRM_BUF *const bufPtr);

#endif

/******************************************************************************/
/* SEQUENCE COUNTER LATEST-VALUE SNAPSHOTS                                    */
/******************************************************************************/
#ifdef RK_CONF_SEQCOUNT
#undef RK_CONF_SEQCOUNT
#endif

#if (RK_CONF_SEQCOUNT == ON)
/*
 * Single-core Cortex-M sequence counter.
 *
 * This is a small latest-value protocol helper for memory that the
 * participating tasks can already access: same-domain RAM, global shared RAM,
 * or an attached RK_SHARED_MEM segment. It is not an inter-domain capability
 * grant and it is not a blocking synchronisation object.
 *
 * Writers must keep RK_CR_ENTER/RK_CR_EXIT active across the whole update:
 * publish an odd sequence, update the payload, then publish an even sequence.
 * The write section must not block, yield, sleep or call services that may
 * invoke the scheduler.
 *
 * Readers copy a snapshot and retry if a writer changed the sequence during
 * the copy. Maskable readers cannot preempt a writer while the sequence is odd.
 * NMI access is outside this contract because PRIMASK does not mask NMI.
 */
typedef struct RK_STRUCT_SEQCOUNT
{
    volatile UINT sequence;
} RK_SEQCOUNT;

#define RK_SEQCOUNT_INITIALIZER { 0U }

RK_FORCE_INLINE
static inline VOID kSeqCountInit(RK_SEQCOUNT *const seqPtr)
{
    seqPtr->sequence = 0U;
}

/* RK_CR_ENTER must already be active in the caller. */
RK_FORCE_INLINE
static inline VOID kSeqCountWriteBegin(RK_SEQCOUNT *const seqPtr)
{
    seqPtr->sequence += 1U; /* Odd: update in progress. */
    RK_DMB
}

/* RK_CR_EXIT must execute immediately after this function. */
RK_FORCE_INLINE
static inline VOID kSeqCountWriteEnd(RK_SEQCOUNT *const seqPtr)
{
    RK_DMB
    seqPtr->sequence += 1U; /* Even: stable snapshot available. */
    RK_DMB
}

RK_FORCE_INLINE
static inline UINT kSeqCountReadBegin(RK_SEQCOUNT const *const seqPtr)
{
    UINT sequence;

    do
    {
        sequence = seqPtr->sequence;
    } while ((sequence & 1U) != 0U);

    RK_DMB
    return (sequence);
}

RK_FORCE_INLINE
static inline RK_BOOL kSeqCountReadRetry(RK_SEQCOUNT const *const seqPtr,
                                         UINT const sequenceBegin)
{
    UINT sequenceEnd;

    RK_DMB
    sequenceEnd = seqPtr->sequence;

    return ((sequenceEnd != sequenceBegin) ? RK_TRUE : RK_FALSE);
}
#endif

#if (RK_CONF_CALLOUT_TIMER == ON)
/******************************************************************************/
/* APPLICATION TIMER                                                          */
/******************************************************************************/
/**
 * @brief Create and arm an application timer from the timer object pool.
 *        The first expiry is ordered by phase + countTicks. A fired one-shot
 *        timer remains created but inactive; there is no public rearm API.
 * @param timerHandlePtr Pointer to a handle variable. The variable must be
 *                       RK_NULL_HANDLE.
 * @param objName       NUL-terminated object name.
 * @param phase Initial phase delay; does not apply to reloads.
 * @param countTicks Period/expiry delay in ticks. Must be non-zero.
 * @param funPtr Callout Function when it expires (callback)
 * @param argsPtr Generic pointer to callout arguments
 * @param reload RK_TIMER_RELOAD for reloading after timer-out.
 *               RK_TIMER_ONESHOT for an one-shot

 * @return       Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_BUFFER_EMPTY
 */
RK_ERR kTimerCreate(RK_HANDLE *const timerHandlePtr,
                    RK_STRING objName,
                    RK_TICK const phase,
                    RK_TICK const countTicks,
                    RK_TIMER_CALLOUT const funPtr,
                    VOID *const argsPtr, RK_OPTION const reload);
RK_ERR kTimerCreateGlobalScope(RK_HANDLE *const timerHandlePtr,
                               RK_STRING objName,
                               RK_TICK const phase,
                               RK_TICK const countTicks,
                               RK_TIMER_CALLOUT const funPtr,
                               VOID *const argsPtr,
                               RK_OPTION const reload);
RK_ERR kTimerCreateDomainScope(RK_HANDLE *const timerHandlePtr,
                               RK_STRING objName,
                               RK_TICK const phase,
                               RK_TICK const countTicks,
                               RK_TIMER_CALLOUT const funPtr,
                               VOID *const argsPtr,
                               RK_OPTION const reload,
                               RK_DOMAIN *const domainPtr);
RK_ERR kTimerDestroy(RK_HANDLE *const timerHandlePtr);

/**
 * @brief       Cancel a created timer. Cancelling an inactive one-shot is
 *              a successful no-op.
 * @param timerHandle Timer handle returned by kTimerCreate().
 * @return      Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_ERROR
 */
RK_ERR kTimerCancel(RK_HANDLE const timerHandle);
#endif

/******************************************************************************/
/* SLEEP AND OTHER TIME RELATED                                               */
/******************************************************************************/
/**
 * @brief       Put the current task to sleep for a number of ticks.
 *              Task switches to SLEEPING state.
 *              This is a relative delay and is not suitable for periodic
 *              tasks because execution time and release jitter accumulate
 *              across iterations. Use kSleepPeriodic()/kSleepRelease() or
 *              kSleepUntil() for periodic task releases.
 * @param ticks Number of ticks to sleep
 * @return      Successful:
 *                                   RK_ERR_SUCCESS
 *                   Errors:
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 *                                   RK_ERR_TASK_INVALID_ST
 *                                   RK_ERR_INVALID_PARAM
 */
RK_ERR kSleepDelay(const RK_TICK ticks);
#define kSleep(t) kSleepDelay(t)

/**
 * @brief     Suspends and release a task periodically, compensating for
 *          drifts and locking phase. Lateness smaller than 1 period
 *          will shorten the time until the next activation, so
 *          phase is kept constant accross calls.
 *          Overruns higher than 1 period cannot be compensated, and
 *          are skipped. Release is scheduled to the next valid time
 *          slot.
 *          Set priorities accordingly.
 *
 * @details
 *          Tasks are kept aligned to a phase grid:
 *          ..., kP | (k+1)P | (k+2)P | ...
 *
 *          If the activation supposed to happen on (k+1)P slot
 *          drifts within the (k+2)P,
 *          task will not execute until somewhere in (k+3)P.
 *          Each skipped release records a task overrun event and increments
 *          the task overrun counter shown by trace `top`.
 *
 *
 * @param   period period in ticks
 * @return  Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kSleepRelease(RK_TICK const period);
#ifndef kSleepPeriodic
#define kSleepPeriodic(t) kSleepRelease(t)
#endif

/**
 * @brief     Suspends a task so it is released periodically.
 *          Differently from kSleepRelease, the reference is local
 *          for each task. Compensation can either shorten the time
 *          between two activations (when overrun is less than 1 period)
 *          or return RK_ERR_ELAPSED_PERIOD immediately. It does not skip.
 *          Each elapsed-period return records an Until overrun event and
 *          increments the task overrun counter shown by trace `top`.
 *
 *
 *  Example: 500 ticks periodic task
 *  @code{c}
 *
 *          VOID task(VOID* args)
 *          {
 *
 *              RK_TICK anchor = kTickGet();
 *              while(1)
 *              {
 *                  work();
 *
 *                  kSleepUntil(&anchor, 500);
 *
 *             }
 *          }
 * @endcode
 *
 * @param   period Period in ticks
 * @param   lastTickPtr Address of the anchored time reference.
 * @return  Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_ELAPSED_PERIOD
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_PARAM
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 */
RK_ERR kSleepUntil(RK_TICK *lastTickPtr, RK_TICK const period);

/**
 * @brief Gets the current number of  ticks
 * @return Global system tick value
 */
RK_TICK kTickGet(VOID);

/**
 * @brief Gets the current number of ticks
 *        in milliseconds
 * @return Global system tick value [ms]
 */
RK_TICK kTickGetMs(VOID);

/**
 * @brief   Active wait for approximately a number of tick periods. Task is
 *          not suspended, and the delay loop does not enter the syscall path.
 *          Time elapsed while preempted is not accumulated.
 * @param   ticks Number of ticks for busy-wait
 * @return  Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 *                                   RK_ERR_INVALID_PARAM
 */
RK_ERR kDelay(RK_TICK const ticks);
#define kBusyDelay(t) kDelay(t)

/******************************************************************************/
/*  MEMORY PARTITION                                                          */
/******************************************************************************/
/**
 * @brief Memory Partition Control Block Initialisation
 * @param kobj Pointer to a  control block
 * @param memPoolPtr Address of a word-aligned pool (typically declared with
 *                   RK_DECLARE_MEM_POOL()).
 * @param blkSize Size of each block in bytes; rounded up to a word internally.
 * @param numBlocks Number of blocks; must be at least 1.
 * @return                  Successful:
 *                                   RK_ERR_SUCCESS
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_OBJ_DOUBLE_INIT
 *                                   RK_ERR_INVALID_PARAM
 */
RK_ERR kMemPartitionInit(RK_MEM_PARTITION *const kobj, VOID *memPoolPtr,
                         ULONG blkSize, const ULONG numBlocks);
RK_ERR kMemPartitionInitGlobalScope(RK_MEM_PARTITION *const kobj,
                                    VOID *memPoolPtr,
                                    ULONG blkSize,
                                    const ULONG numBlocks);
RK_ERR kMemPartitionInitDomainScope(RK_MEM_PARTITION *const kobj,
                                    VOID *memPoolPtr,
                                    ULONG blkSize,
                                    const ULONG numBlocks,
                                    RK_DOMAIN *const domainPtr);
#ifndef RK_DECLARE_MEM_POOL
#define RK_DECLARE_MEM_POOL(TYPE, BUFNAME, N_BLOCKS)                           \
    RK_DECLARE_LOCAL_MEM_POOL(TYPE, BUFNAME, N_BLOCKS)
#endif

#ifndef RK_DECLARE_LOCAL_MEM_POOL
#define RK_DECLARE_LOCAL_MEM_POOL(TYPE, BUFNAME, N_BLOCKS)                     \
    ULONG BUFNAME[N_BLOCKS][RK_TYPE_WORD_COUNT(TYPE)] RK_APP_RAM_ATTR;
#endif

#ifndef RK_DECLARE_GLOBAL_MEM_POOL
#define RK_DECLARE_GLOBAL_MEM_POOL(TYPE, BUFNAME, N_BLOCKS)                    \
    ULONG BUFNAME[N_BLOCKS][RK_TYPE_WORD_COUNT(TYPE)] K_ALIGN(4)               \
        RK_SHARED_RAM_ATTR;
#endif
/**
 * @brief Allocate memory partition from a pool
 * @param kobj Pointer to the partition pool
 * @return Address of a memory block, or NULL on failure
 */
VOID *kMemPartitionAlloc(RK_MEM_PARTITION *const kobj);

/**
 * @brief Free a memory block (Returns it to the pool)
 * @param kobj Pointer to the partition pool
 * @param blockPtr Pointer to the block to free
 * @return              Successful:
 *                                   RK_ERR_SUCCESS
 *                      Unsuccessful:
 *                                   RK_ERR_MEM_FREE       Invalid pointer,
 *                                                         misaligned pointer,
 *                                                         or double free.
 *                      Errors:
 *                                   RK_ERR_OBJ_NULL
 *                                   RK_ERR_INVALID_OBJ
 *                                   RK_ERR_OBJ_NOT_INIT
 */
RK_ERR kMemPartitionFree(RK_MEM_PARTITION *const kobj, VOID *blockPtr);

/******************************************************************************/
/* MISC/HELPERS                                                               */
/******************************************************************************/
/**
 * @brief Returns the kernel version.
 * @return Kernel version as an unsigned integer.
 */
unsigned int kGetVersion(void);
/**
 * @brief Generic error handler
 */
void kErrHandler(RK_FAULT fault);

/**
 * @brief Disables global interrupts
 */
RK_FORCE_INLINE
static inline VOID kDisableIRQ(VOID)
{
    RK_ASM volatile("CPSID I" : : : "memory");
}
/**
 * @brief Enables global interrupts
 */
RK_FORCE_INLINE
static inline VOID kEnableIRQ(VOID)
{
    RK_ASM volatile("CPSIE I" : : : "memory");
}
#if ((RK_CONF_SLEEP_QUEUE == ON) && (RK_CONF_MUTEX == ON) &&                  \
     (RK_CONF_CONDVAR == ON))


/**
 * @brief Condition Variable Wait.
 *        Unlocks associated mutex and suspends task.
 *        If the mutex was successfully unlocked, the function attempts to
 *        reacquire it before returning, including timeout and no-wait returns.
 *        The timeout bounds both cond wait + lock.
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                  Unsuccessful:
 *                                   RK_ERR_TIMEOUT
 *                                   RK_ERR_NOWAIT
 *                                   RK_ERR_INVALID_TIMEOUT
 *                  Errors:
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 *                                   (plus propagated mutex/Sleep Queue errors)
 */
RK_ERR kCondVarWait(RK_HANDLE const cv,
                    RK_HANDLE const mutex,
                    RK_TICK timeout);
/**
 * @brief Wakes a single waiter task on a condition variable.
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                  Unsuccessful:
 *                                   RK_ERR_EMPTY_WAITING_QUEUE
 *                  Errors:
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 *                                   (plus propagated Sleep Queue errors)
 */
RK_ERR kCondVarSignal(RK_HANDLE const cv);
/**
 * @brief Wakes all waiter tasks on a condition variable.
 * @return          Successful:
 *                                   RK_ERR_SUCCESS
 *                  Unsuccessful:
 *                                   RK_ERR_EMPTY_WAITING_QUEUE
 *                  Errors:
 *                                   RK_ERR_INVALID_ISR_PRIMITIVE
 *                                   (plus propagated Sleep Queue errors)
 */
RK_ERR kCondVarBroadcast(RK_HANDLE const cv);
#endif
/******************************************************************************/
/* CONVENIENCE MACROS                                                         */
/******************************************************************************/
/* Running Task Get */
extern RK_TCB *RK_gRunPtr;
/**
 * @brief Convert ticks to milliseconds
 * @param ticks Number of ticks
 * @return Equivalent number of milliseconds
 */
#ifndef RK_TICKS_TO_MS
#define RK_TICKS_TO_MS(ticks) ((ticks) * RK_TICK_INTERVAL_MS)
#endif

/**
 * @brief Convert milliseconds to ticks
 * @param ms Number of milliseconds
 * @return Equivalent number of ticks
 */
/*
making a inline function is safer given side-effects with t++
*/
#ifndef RK_MS_TO_TICKS_DEFINED
#define RK_MS_TO_TICKS_DEFINED
static inline RK_BOOL K_MS_TO_TICKS_IS_ZERO(RK_TICK t)
{
    return ((t / RK_TICK_INTERVAL_MS) == 0U);
}

static inline RK_TICK RK_MS_TO_TICKS(RK_TICK ms)
{
    if (ms == 0U)
    {
#if (RK_CONF_ERR_CHECK == ON)
        K_ERR_HANDLER(RK_FAULT_INVALID_PARAM);
#endif
        return (0U);
    }

#if (RK_CONF_ROUND_UP_MS_TO_TICKS == ON)
    if (K_MS_TO_TICKS_IS_ZERO(ms) == RK_TRUE)
    {
        return (1U);
    }
#endif

    return ((RK_TICK)(ms / RK_TICK_INTERVAL_MS));
}
#endif
/**
 * @brief Get active task ID
 */
#ifndef RK_RUNNING_TID
#define RK_RUNNING_TID (kTaskGetID(kTaskGetRunningHandle()))
#endif
#ifndef RK_RUNNING_PID
#define RK_RUNNING_PID RK_RUNNING_TID
#endif

/**
 * @brief Get active task effective priority
 */
#ifndef RK_RUNNING_PRIO
#define RK_RUNNING_PRIO (kTaskGetPrio(kTaskGetRunningHandle()))
#endif

/**
 * @brief Get active task nominal (real/assigned) priority
 */
#ifndef RK_RUNNING_NOM_PRIO
#define RK_RUNNING_NOM_PRIO (kTaskGetNomPrio(kTaskGetRunningHandle()))
#endif
/**
 * @brief Get active task handle
 */
#ifndef RK_RUNNING_HANDLE
#define RK_RUNNING_HANDLE (kTaskGetRunningHandle())
#endif
/**
 * @brief Get active task name
 */
#ifndef RK_RUNNING_NAME
#define RK_RUNNING_NAME (kTaskGetRunningName())
#endif
/**
 * @brief Get a task ID
 * @param taskHandle Task Handle
 */
#ifndef RK_TASK_ID
#define RK_TASK_ID(taskHandle) (kTaskGetID(taskHandle))
#endif

/**
 * @brief Get a task priority
 * @param taskHandle Task Handle
 */
#ifndef RK_TASK_PRIO
#define RK_TASK_PRIO(taskHandle) (kTaskGetPrio(taskHandle))
#endif

#endif /* KAPI_H */
