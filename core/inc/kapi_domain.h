/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_API_DOMAIN_H
#define RK_API_DOMAIN_H

#include <kapi_app.h>

#ifdef __cplusplus
extern "C" {
#endif

RK_DOMAIN *kApplicationDomainGet(VOID);

RK_ERR kDomainInit(RK_DOMAIN *const domainPtr,
                   BYTE *const regionBasePtr,
                   ULONG const regionBytes,
                   RK_STRING domainName);
VOID *kDomainAlloc(RK_DOMAIN *const domainPtr,
                   ULONG const nBytes,
                   ULONG const alignBytes);
RK_ERR kTaskInitDomain(RK_TASK_HANDLE *taskHandlePtr,
                       const RK_TASKENTRY taskFunc,
                       VOID *argsPtr,
                       RK_STRING taskName,
                       RK_STACK *const stackBufPtr,
                       const ULONG stackSize,
                       const RK_PRIO priority,
                       const RK_OPTION preempt,
                       RK_DOMAIN *const domainPtr);
RK_ERR kTaskInitIsolated(RK_TASK_HANDLE *taskHandlePtr,
                         const RK_TASKENTRY taskFunc,
                         VOID *argsPtr,
                         RK_STRING taskName,
                         RK_STACK *const stackBufPtr,
                         const ULONG stackSize,
                         const RK_PRIO priority,
                         const RK_OPTION preempt);
RK_ERR kSharedMemCreate(RK_SHARED_MEM_HANDLE *const sharedMemHandlePtr,
                        RK_STRING objName,
                        VOID *const regionBasePtr,
                        ULONG const regionBytes);
RK_ERR kSharedMemAttach(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                        RK_DOMAIN *const domainPtr);
RK_ERR kSharedMemDetach(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                        RK_DOMAIN *const domainPtr);
RK_ERR kSharedMemDestroy(RK_SHARED_MEM_HANDLE *const sharedMemHandlePtr);
RK_ERR kSharedMemGet(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                     VOID **const regionBasePPtr,
                     ULONG *const regionBytesPtr);

#ifndef RK_ISOLATED_TASK_STACK_ATTR
#define RK_ISOLATED_TASK_STACK_ATTR(NWORDS)                                   \
    RK_STACK_ALIGN(NWORDS) RK_SECTION_TASK_STACK
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

#ifndef RK_DECLARE_DOMAIN
#define RK_DECLARE_DOMAIN(DOMAIN, RAMBUF, NBYTES)                             \
    BYTE RAMBUF[NBYTES] RK_DOMAIN_RAM_ATTR(NBYTES);                           \
    RK_DOMAIN DOMAIN RK_DOMAIN_DESC_ATTR;
#endif

#ifndef RK_DECLARE_DOMAIN_RAM
#define RK_DECLARE_DOMAIN_RAM(RAM_LAYOUT, ...)                                 \
    typedef struct                                                             \
    {                                                                         \
        __VA_ARGS__                                                           \
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
#define RK_DOMAIN_WINDOW_STATIC_ASSERT_(RAM_LAYOUT, NBYTES)                   \
    _Static_assert(sizeof(RAM_LAYOUT) <= (NBYTES),                            \
                   "domain RAM layout must fit its MPU window");             \
    _Static_assert((NBYTES) >= 32U,                                           \
                   "domain RAM window must be at least 32 bytes");           \
    _Static_assert(((NBYTES) & ((NBYTES) - 1U)) == 0U,                        \
                   "domain RAM window must be a power of two");              \
    _Static_assert(_Alignof(RAM_LAYOUT) <= (NBYTES),                          \
                   "domain RAM layout alignment must fit its MPU window")
#endif

#ifndef RK_DECLARE_TYPED_DOMAIN
#define RK_DECLARE_TYPED_DOMAIN(DOMAIN, RAMBUF, RAM_LAYOUT, NBYTES)           \
    RK_DOMAIN_WINDOW_STATIC_ASSERT_(RAM_LAYOUT, NBYTES);                      \
    typedef union                                                             \
    {                                                                         \
        BYTE bytes[NBYTES];                                                   \
        RAM_LAYOUT typed;                                                     \
    } RAMBUF##_RK_DOMAIN_WINDOW;                                              \
    RAMBUF##_RK_DOMAIN_WINDOW RAMBUF RK_DOMAIN_RAM_ATTR(NBYTES);              \
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
#define RK_DOMAIN_INIT_TYPED(DOMAINPTR, RAMBUF, NAME)                        \
    kDomainInit((DOMAINPTR), RK_DOMAIN_WINDOW_BASE(RAMBUF),                  \
                RK_DOMAIN_WINDOW_BYTES(RAMBUF), (NAME))
#endif

#ifndef RK_DECLARE_DOMAIN_TASK
#define RK_DECLARE_DOMAIN_TASK(HANDLE, TASKENTRY)                             \
    VOID TASKENTRY(VOID *args);                                               \
    RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_DOMAIN_TASK_STACK
#define RK_DECLARE_DOMAIN_TASK_STACK(STACKBUF, NWORDS)                        \
    RK_STACK STACKBUF[NWORDS] RK_TASK_STACK_ATTR(NWORDS);
#endif

#ifndef RK_DECLARE_ISOLATED_TASK
#define RK_DECLARE_ISOLATED_TASK(HANDLE, TASKENTRY, STACKBUF, NWORDS)         \
    VOID TASKENTRY(VOID *args);                                               \
    RK_STACK STACKBUF[NWORDS] RK_ISOLATED_TASK_STACK_ATTR(NWORDS);            \
    RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

#ifndef RK_DOMAIN_ALLOC
#define RK_DOMAIN_ALLOC(DOMAINPTR, TYPE)                                      \
    ((TYPE *)kDomainAlloc((DOMAINPTR), sizeof(TYPE), (ULONG)_Alignof(TYPE)))
#endif

#ifndef RK_DOMAIN_ALLOC_ARRAY
#define RK_DOMAIN_ALLOC_ARRAY(DOMAINPTR, TYPE, COUNT)                         \
    ((TYPE *)kDomainAlloc((DOMAINPTR),                                       \
                          sizeof(TYPE) * (ULONG)(COUNT),                     \
                          (ULONG)_Alignof(TYPE)))
#endif

#ifdef __cplusplus
}
#endif

#endif /* RK_API_DOMAIN_H */
