/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RK_API_MODULE_H
#define RK_API_MODULE_H

#include <kapi_app.h>

#ifdef __cplusplus
extern "C" {
#endif

RK_MODULE *kApplicationModuleGet(VOID);

RK_ERR kModuleInit(RK_MODULE *const modulePtr,
                   BYTE *const regionBasePtr,
                   ULONG const regionBytes,
                   RK_STRING moduleName);
VOID *kModuleAlloc(RK_MODULE *const modulePtr,
                   ULONG const nBytes,
                   ULONG const alignBytes);
RK_STACK *kModuleStackAlloc(RK_MODULE *const modulePtr,
                            ULONG const stackWords);
RK_ERR kModuleTaskInit(RK_MODULE *const modulePtr,
                       RK_TASK_HANDLE *taskHandlePtr,
                       const RK_TASKENTRY taskFunc,
                       VOID *argsPtr,
                       RK_STRING taskName,
                       const ULONG stackWords,
                       const RK_PRIO priority,
                       const RK_OPTION preempt);
RK_ERR kTaskInitModule(RK_TASK_HANDLE *taskHandlePtr,
                       const RK_TASKENTRY taskFunc,
                       VOID *argsPtr,
                       RK_STRING taskName,
                       RK_STACK *const stackBufPtr,
                       const ULONG stackSize,
                       const RK_PRIO priority,
                       const RK_OPTION preempt,
                       RK_MODULE *const modulePtr);
RK_ERR kTaskInitIsolated(RK_TASK_HANDLE *taskHandlePtr,
                         const RK_TASKENTRY taskFunc,
                         VOID *argsPtr,
                         RK_STRING taskName,
                         RK_STACK *const stackBufPtr,
                         const ULONG stackSize,
                         const RK_PRIO priority,
                         const RK_OPTION preempt);
RK_ERR kSharedMemCreate(RK_SHARED_MEM_HANDLE *const sharedMemHandlePtr,
                        VOID *const regionBasePtr,
                        ULONG const regionBytes);
RK_ERR kSharedMemAttach(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                        RK_MODULE *const modulePtr);
RK_ERR kSharedMemDetach(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                        RK_MODULE *const modulePtr);
RK_ERR kSharedMemDestroy(RK_SHARED_MEM_HANDLE *const sharedMemHandlePtr);
RK_ERR kSharedMemGet(RK_SHARED_MEM_HANDLE const sharedMemHandle,
                     VOID **const regionBasePPtr,
                     ULONG *const regionBytesPtr);

#ifndef RK_ISOLATED_TASK_STACK_ATTR
#define RK_ISOLATED_TASK_STACK_ATTR(NWORDS)                                   \
    RK_STACK_ALIGN(NWORDS) RK_SECTION_MODULE_BSS
#endif

#ifndef RK_MODULE_RAM_ATTR
#define RK_MODULE_RAM_ATTR(NBYTES) K_ALIGN(NBYTES) RK_SECTION_MODULE_BSS
#endif

#ifndef RK_KERNEL_RAM_ATTR
#define RK_KERNEL_RAM_ATTR K_ALIGN(4) RK_SECTION_NAMED(".rk_kernel_bss")
#endif

#ifndef RK_MODULE_DESC_ATTR
#define RK_MODULE_DESC_ATTR RK_KERNEL_RAM_ATTR
#endif

#ifndef RK_DECLARE_MODULE
#define RK_DECLARE_MODULE(MODULE, RAMBUF, NBYTES)                             \
    BYTE RAMBUF[NBYTES] RK_MODULE_RAM_ATTR(NBYTES);                           \
    RK_MODULE MODULE RK_MODULE_DESC_ATTR;
#endif

#ifndef RK_DECLARE_MODULE_TASK
#define RK_DECLARE_MODULE_TASK(HANDLE, TASKENTRY)                             \
    VOID TASKENTRY(VOID *args);                                               \
    RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

#ifndef RK_DECLARE_ISOLATED_TASK
#define RK_DECLARE_ISOLATED_TASK(HANDLE, TASKENTRY, STACKBUF, NWORDS)         \
    VOID TASKENTRY(VOID *args);                                               \
    RK_STACK STACKBUF[NWORDS] RK_ISOLATED_TASK_STACK_ATTR(NWORDS);            \
    RK_DECLARE_GLOBAL_TASK_HANDLE(HANDLE)
#endif

#ifndef RK_MODULE_ALLOC
#define RK_MODULE_ALLOC(MODULEPTR, TYPE)                                      \
    ((TYPE *)kModuleAlloc((MODULEPTR), sizeof(TYPE), (ULONG)_Alignof(TYPE)))
#endif

#ifndef RK_MODULE_ALLOC_ARRAY
#define RK_MODULE_ALLOC_ARRAY(MODULEPTR, TYPE, COUNT)                         \
    ((TYPE *)kModuleAlloc((MODULEPTR),                                       \
                          sizeof(TYPE) * (ULONG)(COUNT),                     \
                          (ULONG)_Alignof(TYPE)))
#endif

#ifndef RK_MODULE_ALLOC_STACK
#define RK_MODULE_ALLOC_STACK(MODULEPTR, NWORDS)                             \
    kModuleStackAlloc((MODULEPTR), (NWORDS))
#endif

#ifdef __cplusplus
}
#endif

#endif /* RK_API_MODULE_H */
