/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
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
RK_STACK *kDomainStackAlloc(RK_DOMAIN *const domainPtr,
                            ULONG const stackWords);
RK_ERR kDomainTaskInit(RK_DOMAIN *const domainPtr,
                       RK_TASK_HANDLE *taskHandlePtr,
                       const RK_TASKENTRY taskFunc,
                       VOID *argsPtr,
                       RK_STRING taskName,
                       const ULONG stackWords,
                       const RK_PRIO priority,
                       const RK_OPTION preempt);
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
    RK_STACK_ALIGN(NWORDS) RK_SECTION_DOMAIN_BSS
#endif

#ifndef RK_DOMAIN_RAM_ATTR
#define RK_DOMAIN_RAM_ATTR(NBYTES) K_ALIGN(NBYTES) RK_SECTION_DOMAIN_BSS
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

#ifndef RK_DECLARE_DOMAIN_TASK
#define RK_DECLARE_DOMAIN_TASK(HANDLE, TASKENTRY)                             \
    VOID TASKENTRY(VOID *args);                                               \
    RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
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

#ifndef RK_DOMAIN_ALLOC_STACK
#define RK_DOMAIN_ALLOC_STACK(DOMAINPTR, NWORDS)                             \
    kDomainStackAlloc((DOMAINPTR), (NWORDS))
#endif

#ifdef __cplusplus
}
#endif

#endif /* RK_API_DOMAIN_H */
