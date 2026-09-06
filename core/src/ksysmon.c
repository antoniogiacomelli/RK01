/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * File intent:
 *   Lightweight UART command monitor. It keeps one intrusive list per named
 *   kernel-object type and prints bounded snapshots on request. It is not an
 *   event trace recorder.
 */

#define RK_SOURCE_CODE

#include <ksysmon.h>

#if (RK_CONF_SYSMON == ON)

#include <kapi.h>
#include <kconsole.h>
#include <kdynobjs.h>
#include <klist.h>
#include <klogger.h>
#include <ksch.h>
#include <kstring.h>
#include <ksyscall.h>
#include <ktimer.h>
#include <stdio.h>

typedef enum
{
    RK_SYSMON_FAMILY_MEM = 0U,
    RK_SYSMON_FAMILY_SEMA,
    RK_SYSMON_FAMILY_SLEEPQ,
    RK_SYSMON_FAMILY_MUTEX,
    RK_SYSMON_FAMILY_MESGQ,
    RK_SYSMON_FAMILY_MRM,
    RK_SYSMON_FAMILY_TIMER,
    RK_SYSMON_FAMILY_SHARED_MEM,
    RK_SYSMON_FAMILY_COUNT
} RK_SYSMON_FAMILY_;

typedef struct
{
    RK_TID tid;
    CHAR name[RK_OBJ_MAX_NAME_LEN];
    CHAR moduleName[RK_NAME_SIZE];
    RK_TASK_STATUS status;
    RK_PRIO priority;
    RK_PRIO prioNominal;
    ULONG runCnt;
    ULONG ownedMutexes;
    ULONG overrunCount;
    RK_TASK_EVENT eventCurr;
    RK_TASK_EVENT eventReq;
    RK_OPTION eventOpt;
    RK_STACK stackFreeWords;
    RK_STACK stackSizeWords;
    RK_BOOL privileged;
} RK_SYSMON_TASK_ROW_;

typedef struct
{
    RK_ID objID;
    CHAR objName[RK_NAME_SIZE];
    CHAR moduleName[RK_NAME_SIZE];
    CHAR ownerName[RK_OBJ_MAX_NAME_LEN];
    VOID const *objPtr;
    RK_OBJ_SCOPE scope;
    RK_BOOL ownerValid;
    RK_TID ownerTid;
    ULONG value0;
    ULONG value1;
    ULONG value2;
    ULONG value3;
    ULONG value4;
} RK_SYSMON_OBJECT_ROW_;

static RK_LIST sysMonObjectLists[RK_SYSMON_FAMILY_COUNT];
static RK_BOOL sysMonObjectListsInit;
static RK_TASK_HANDLE sysMonTaskHandle;
static RK_STACK sysMonStack[RK_CONF_SYSMON_STACKSIZE]
    RK_PRIVILEGED_TASK_STACK_ATTR(RK_CONF_SYSMON_STACKSIZE);
static CHAR sysMonLine[RK_CONF_SYSMON_LINE_LEN];
static UINT sysMonLineLen;
static RK_BOOL sysMonDropLf;
static RK_BOOL sysMonDiagnosisActive;
static RK_BOOL sysMonSavedLogNormalOutput;
static RK_SYSMON_TASK_ROW_ sysMonTaskRow;
static RK_SYSMON_OBJECT_ROW_
    sysMonObjectRows[RK_CONF_SYSMON_SNAPSHOT_MAX];

static VOID kSysMonTask_(VOID *args);

static VOID kSysMonNameCopy_(CHAR *const dstPtr,
                             ULONG const dstBytes,
                             CHAR const *srcPtr)
{
    ULONG i = 0UL;

    if ((dstPtr == NULL) || (dstBytes == 0UL))
    {
        return;
    }

    if (srcPtr == NULL)
    {
        srcPtr = "";
    }

    while (((i + 1UL) < dstBytes) && (srcPtr[i] != '\0'))
    {
        dstPtr[i] = srcPtr[i];
        i++;
    }
    dstPtr[i] = '\0';
}

#if (RK_CONF_MRM == ON)
static VOID kSysMonNameCopyWithSuffix_(CHAR *const dstPtr,
                                       CHAR const *const srcPtr,
                                       CHAR const suffix)
{
    UINT i = 0U;

    if ((dstPtr == NULL) || (srcPtr == NULL))
    {
        return;
    }

    for (; (i < (RK_NAME_SIZE - 2U)) && (srcPtr[i] != '\0'); i++)
    {
        dstPtr[i] = srcPtr[i];
    }

    dstPtr[i] = suffix;
    dstPtr[i + 1U] = '\0';
}
#endif

static CHAR const *kSysMonStatusName_(RK_TASK_STATUS const status)
{
    switch (status)
    {
        case RK_TCB_INITIALISED:
            return ("INIT");
        case RK_READY:
            return ("RDY");
        case RK_RUNNING:
            return ("RUN");
        case RK_SLEEPING:
            return ("SLEEP");
        case RK_SLEEPING_EV_FLAG:
            return ("EVFLAG");
        case RK_BLOCKED:
            return ("BLKD");
        case RK_SENDING:
            return ("SEND");
        case RK_RECEIVING:
            return ("RECV");
        case RK_SLEEPING_DELAY:
            return ("SLPDLY");
        case RK_SLEEPING_RELEASE:
            return ("SLPREL");
        case RK_SLEEPING_UNTIL:
            return ("SLPUNT");
        case RK_SLEEPQ_BLOCKED:
            return ("SLPQ");
        case RK_PENDING:
            return ("PEND");
        case RK_TASK_FAULT_PENDING:
            return ("FAULT");
        case RK_TASK_TERMINATED:
            return ("TERM");
        default:
            return ("?");
    }
}

static CHAR const *kSysMonScopeName_(RK_OBJ_SCOPE const scope)
{
    switch (scope)
    {
        case RK_SCOPE_MODULE_LOCAL:
            return ("module");
        case RK_SCOPE_KERNEL_GLOBAL:
            return ("global");
        case RK_SCOPE_UNASSIGNED:
            return ("raw");
        default:
            return ("?");
    }
}

static CHAR const *kSysMonObjTypeName_(RK_ID const objID)
{
    switch (objID)
    {
        case RK_MEMALLOC_KOBJ_ID:
            return ("mem");
        case RK_SEMAPHORE_KOBJ_ID:
            return ("sema");
        case RK_SLEEPQ_KOBJ_ID:
            return ("sleepq");
        case RK_MUTEX_KOBJ_ID:
            return ("mutex");
        case RK_MESGQQUEUE_KOBJ_ID:
            return ("queue");
        case RK_MRM_KOBJ_ID:
            return ("mrm");
        case RK_TIMER_KOBJ_ID:
            return ("timer");
        case RK_SHARED_MEM_KOBJ_ID:
            return ("shared");
        default:
            return ("?");
    }
}

static INT kSysMonFamilyIndex_(RK_ID const objID)
{
    switch (objID)
    {
        case RK_MEMALLOC_KOBJ_ID:
            return ((INT)RK_SYSMON_FAMILY_MEM);
        case RK_SEMAPHORE_KOBJ_ID:
            return ((INT)RK_SYSMON_FAMILY_SEMA);
        case RK_SLEEPQ_KOBJ_ID:
            return ((INT)RK_SYSMON_FAMILY_SLEEPQ);
        case RK_MUTEX_KOBJ_ID:
            return ((INT)RK_SYSMON_FAMILY_MUTEX);
        case RK_MESGQQUEUE_KOBJ_ID:
            return ((INT)RK_SYSMON_FAMILY_MESGQ);
        case RK_MRM_KOBJ_ID:
            return ((INT)RK_SYSMON_FAMILY_MRM);
        case RK_TIMER_KOBJ_ID:
            return ((INT)RK_SYSMON_FAMILY_TIMER);
        case RK_SHARED_MEM_KOBJ_ID:
            return ((INT)RK_SYSMON_FAMILY_SHARED_MEM);
        default:
            return (-1);
    }
}

static RK_ID kSysMonFamilyObjId_(RK_SYSMON_FAMILY_ const family)
{
    switch (family)
    {
        case RK_SYSMON_FAMILY_MEM:
            return (RK_MEMALLOC_KOBJ_ID);
        case RK_SYSMON_FAMILY_SEMA:
            return (RK_SEMAPHORE_KOBJ_ID);
        case RK_SYSMON_FAMILY_SLEEPQ:
            return (RK_SLEEPQ_KOBJ_ID);
        case RK_SYSMON_FAMILY_MUTEX:
            return (RK_MUTEX_KOBJ_ID);
        case RK_SYSMON_FAMILY_MESGQ:
            return (RK_MESGQQUEUE_KOBJ_ID);
        case RK_SYSMON_FAMILY_MRM:
            return (RK_MRM_KOBJ_ID);
        case RK_SYSMON_FAMILY_TIMER:
            return (RK_TIMER_KOBJ_ID);
        case RK_SYSMON_FAMILY_SHARED_MEM:
            return (RK_SHARED_MEM_KOBJ_ID);
        default:
            return (RK_INVALID_KOBJ);
    }
}

static VOID kSysMonObjectListsEnsure_(VOID)
{
    if (sysMonObjectListsInit == RK_TRUE)
    {
        return;
    }

    RK_CR_AREA
    RK_CR_ENTER
    if (sysMonObjectListsInit != RK_TRUE)
    {
        for (UINT i = 0U; i < (UINT)RK_SYSMON_FAMILY_COUNT; i++)
        {
            (VOID)kListInit(&sysMonObjectLists[i]);
        }
        sysMonObjectListsInit = RK_TRUE;
    }
    RK_CR_EXIT
}

static RK_ERR kSysMonResolveObject_(RK_HANDLE const objHandle,
                                    RK_KOBJ **const objPPtr)
{
    VOID *objPtr = NULL;

    if ((objHandle == RK_NULL_HANDLE) || (objPPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }
    *objPPtr = NULL;

    if (kDynObjHandleIsEncoded(objHandle) == RK_TRUE)
    {
        RK_ERR const err = kDynObjResolveAnyHandle(objHandle, &objPtr);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
    }
    else
    {
        objPtr = (VOID *)(UINTPTR)objHandle;
    }

    if (objPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    *objPPtr = (RK_KOBJ *)objPtr;
    return (RK_ERR_SUCCESS);
}

static RK_STACK kSysMonStackFreeWords_(RK_TCB const *const taskPtr)
{
    if ((taskPtr == NULL) || (taskPtr->stackBufPtr == NULL) ||
        (taskPtr->stackSize == 0U))
    {
        return (0U);
    }

    for (RK_STACK i = 1U; i < taskPtr->stackSize; i++)
    {
        if (taskPtr->stackBufPtr[i] != RK_STACK_PATTERN)
        {
            return (i - 1U);
        }
    }

    return (taskPtr->stackSize - 1U);
}

static VOID kSysMonFillTaskRow_(RK_TCB const *const taskPtr,
                                RK_SYSMON_TASK_ROW_ *const rowPtr)
{
    if ((taskPtr == NULL) || (rowPtr == NULL))
    {
        return;
    }

    rowPtr->tid = taskPtr->tid;
    kSysMonNameCopy_(rowPtr->name, sizeof(rowPtr->name), taskPtr->taskName);
    if (taskPtr->modulePtr != NULL)
    {
        kSysMonNameCopy_(rowPtr->moduleName, sizeof(rowPtr->moduleName),
                         taskPtr->modulePtr->moduleName);
    }
    else
    {
        kSysMonNameCopy_(rowPtr->moduleName, sizeof(rowPtr->moduleName), "-");
    }
    rowPtr->status = taskPtr->status;
    rowPtr->priority = taskPtr->priority;
    rowPtr->prioNominal = taskPtr->prioNominal;
    rowPtr->runCnt = taskPtr->runCnt;
    rowPtr->overrunCount = taskPtr->overrunCount;
    rowPtr->eventCurr = taskPtr->flagsCurr;
    rowPtr->eventReq = taskPtr->flagsReq;
    rowPtr->eventOpt = taskPtr->flagsOpt;
    rowPtr->stackFreeWords = kSysMonStackFreeWords_(taskPtr);
    rowPtr->stackSizeWords = taskPtr->stackSize;
    rowPtr->privileged =
        (((taskPtr->savedControl & 0x1UL) == 0UL) ? RK_TRUE : RK_FALSE);
#if (RK_CONF_MUTEX == ON)
    rowPtr->ownedMutexes = taskPtr->ownedMutexList.size;
#else
    rowPtr->ownedMutexes = 0UL;
#endif
}

static UINT kSysMonTaskCount_(VOID)
{
    UINT count = 0U;

    RK_CR_AREA
    RK_CR_ENTER
    for (RK_TID tid = 0U; tid < RK_NTHREADS; tid++)
    {
        RK_TCB const *const taskPtr = RK_gTaskHandleByPid[tid];
        if ((taskPtr != NULL) && (taskPtr->init == RK_TRUE) &&
            (taskPtr->tid == tid))
        {
            count++;
        }
    }
    RK_CR_EXIT

    return (count);
}

static RK_BOOL kSysMonSnapshotTask_(RK_TID const tid,
                                    RK_SYSMON_TASK_ROW_ *const rowPtr)
{
    RK_BOOL valid = RK_FALSE;

    if (rowPtr == NULL)
    {
        return (RK_FALSE);
    }

    RK_CR_AREA
    RK_CR_ENTER
    RK_TCB const *const taskPtr = RK_gTaskHandleByPid[tid];
    if ((taskPtr != NULL) && (taskPtr->init == RK_TRUE) &&
        (taskPtr->tid == tid))
    {
        kSysMonFillTaskRow_(taskPtr, rowPtr);
        valid = RK_TRUE;
    }
    RK_CR_EXIT

    return (valid);
}

static VOID kSysMonFillObjectCommon_(RK_KOBJ const *const objPtr,
                                     RK_SYSMON_OBJECT_ROW_ *const rowPtr)
{
    rowPtr->objID = objPtr->objID;
    rowPtr->objPtr = objPtr;
    rowPtr->scope = objPtr->scope;
    kSysMonNameCopy_(rowPtr->objName, sizeof(rowPtr->objName),
                     (objPtr->objName[0] != '\0')
                         ? objPtr->objName
                         : kSysMonObjTypeName_(objPtr->objID));
    if ((objPtr->scope == RK_SCOPE_MODULE_LOCAL) &&
        (objPtr->ownerModulePtr != NULL))
    {
        kSysMonNameCopy_(rowPtr->moduleName, sizeof(rowPtr->moduleName),
                         objPtr->ownerModulePtr->moduleName);
    }
    else
    {
        kSysMonNameCopy_(rowPtr->moduleName, sizeof(rowPtr->moduleName), "-");
    }
    kSysMonNameCopy_(rowPtr->ownerName, sizeof(rowPtr->ownerName), "-");
}

static VOID kSysMonFillObjectRow_(RK_KOBJ const *const objPtr,
                                  RK_SYSMON_OBJECT_ROW_ *const rowPtr)
{
    if ((objPtr == NULL) || (rowPtr == NULL))
    {
        return;
    }

    RK_MEMSET(rowPtr, 0, sizeof(*rowPtr));
    kSysMonFillObjectCommon_(objPtr, rowPtr);

    switch (objPtr->objID)
    {
        case RK_MEMALLOC_KOBJ_ID:
        {
            RK_MEM_PARTITION const *const memPtr =
                (RK_MEM_PARTITION const *)objPtr;
            rowPtr->value0 = memPtr->nMaxBlocks - memPtr->nFreeBlocks;
            rowPtr->value1 = memPtr->nMaxBlocks;
            rowPtr->value2 = memPtr->blkSize;
            rowPtr->value3 = memPtr->waitingQueue.size;
            break;
        }
#if (RK_CONF_SEMAPHORE == ON)
        case RK_SEMAPHORE_KOBJ_ID:
        {
            RK_SEMAPHORE const *const semaPtr =
                (RK_SEMAPHORE const *)objPtr;
            rowPtr->value0 = semaPtr->value;
            rowPtr->value1 = semaPtr->maxValue;
            rowPtr->value2 = semaPtr->waitingQueue.size;
            break;
        }
#endif
#if (RK_CONF_SLEEP_QUEUE == ON)
        case RK_SLEEPQ_KOBJ_ID:
        {
            RK_SLEEP_QUEUE const *const sleepqPtr =
                (RK_SLEEP_QUEUE const *)objPtr;
            rowPtr->value0 = sleepqPtr->waitingQueue.size;
            break;
        }
#endif
#if (RK_CONF_MUTEX == ON)
        case RK_MUTEX_KOBJ_ID:
        {
            RK_MUTEX const *const mutexPtr = (RK_MUTEX const *)objPtr;
            rowPtr->value0 = mutexPtr->lock;
            rowPtr->value1 = mutexPtr->protocol;
            rowPtr->value2 = mutexPtr->waitingQueue.size;
            rowPtr->value3 = mutexPtr->ownerFaulted;
            if (mutexPtr->ownerPtr != NULL)
            {
                rowPtr->ownerValid = RK_TRUE;
                rowPtr->ownerTid = mutexPtr->ownerPtr->tid;
                kSysMonNameCopy_(rowPtr->ownerName, sizeof(rowPtr->ownerName),
                                 mutexPtr->ownerPtr->taskName);
            }
            break;
        }
#endif
#if (RK_CONF_MESG_QUEUE == ON)
        case RK_MESGQQUEUE_KOBJ_ID:
        {
            RK_MESG_QUEUE const *const queuePtr =
                (RK_MESG_QUEUE const *)objPtr;
            rowPtr->value0 = queuePtr->ringBuf.nFull;
            rowPtr->value1 = queuePtr->ringBuf.maxBuf;
            rowPtr->value2 = queuePtr->waitingReceivers.size;
            rowPtr->value3 = queuePtr->waitingSenders.size;
            rowPtr->value4 = queuePtr->broadcastReceivers;
            break;
        }
#endif
#if (RK_CONF_MRM == ON)
        case RK_MRM_KOBJ_ID:
        {
            RK_MRM const *const mrmPtr = (RK_MRM const *)objPtr;
            rowPtr->value0 = mrmPtr->size;
            rowPtr->value1 = (mrmPtr->currBufPtr != NULL) ? 1UL : 0UL;
            rowPtr->value2 = (mrmPtr->currBufPtr != NULL)
                                 ? mrmPtr->currBufPtr->nUsers
                                 : 0UL;
            break;
        }
#endif
#if (RK_CONF_CALLOUT_TIMER == ON)
        case RK_TIMER_KOBJ_ID:
        {
            RK_TIMER const *const timerPtr = (RK_TIMER const *)objPtr;
            rowPtr->value0 = kTimeoutNodeIsArmed(&timerPtr->timeoutNode);
            rowPtr->value1 = timerPtr->reload;
            rowPtr->value2 = timerPtr->period;
            rowPtr->value3 = timerPtr->phase;
            break;
        }
#endif
        case RK_SHARED_MEM_KOBJ_ID:
        {
            RK_SHARED_MEM const *const sharedPtr =
                (RK_SHARED_MEM const *)objPtr;
            rowPtr->value0 = sharedPtr->region.regionBytes;
            rowPtr->value1 = sharedPtr->attachCount;
            break;
        }
        default:
            break;
    }
}

static UINT kSysMonSnapshotObjects_(RK_ID const objID,
                                    UINT *const totalPtr)
{
    UINT count = 0U;
    INT const family = kSysMonFamilyIndex_(objID);

    if (totalPtr != NULL)
    {
        *totalPtr = 0U;
    }

    if (family < 0)
    {
        return (0U);
    }

    kSysMonObjectListsEnsure_();

    RK_CR_AREA
    RK_CR_ENTER
    RK_LIST const *const listPtr = &sysMonObjectLists[family];
    if (totalPtr != NULL)
    {
        *totalPtr = (UINT)listPtr->size;
    }

    RK_NODE const *nodePtr = listPtr->listDummy.nextPtr;
    while ((nodePtr != &listPtr->listDummy) &&
           (count < RK_CONF_SYSMON_SNAPSHOT_MAX))
    {
        RK_KOBJ const *const objPtr =
            K_GET_CONTAINER_ADDR(nodePtr, RK_KOBJ, sysMonNode);
        if ((objPtr->init == RK_TRUE) && (objPtr->objID == objID))
        {
            kSysMonFillObjectRow_(objPtr, &sysMonObjectRows[count]);
            count++;
        }
        nodePtr = nodePtr->nextPtr;
    }
    RK_CR_EXIT

    return (count);
}

VOID kSysMonObjectRegister(RK_KOBJ *const objPtr, RK_ID const objID)
{
    INT const family = kSysMonFamilyIndex_(objID);

    if ((objPtr == NULL) || (family < 0))
    {
        return;
    }

    kSysMonObjectListsEnsure_();

    RK_CR_AREA
    RK_CR_ENTER
    if (objPtr->sysMonListed == RK_TRUE)
    {
        RK_CR_EXIT
        return;
    }

    objPtr->sysMonNode.nextPtr = NULL;
    objPtr->sysMonNode.prevPtr = NULL;
    if (objPtr->objName[0] == '\0')
    {
        kSysMonNameCopy_(objPtr->objName, RK_NAME_SIZE,
                         kSysMonObjTypeName_(objID));
    }

    if (kListAddTail(&sysMonObjectLists[family],
                     &objPtr->sysMonNode) == RK_ERR_SUCCESS)
    {
        objPtr->sysMonListed = RK_TRUE;
    }
    RK_CR_EXIT
}

VOID kSysMonObjectUnregister(RK_KOBJ *const objPtr)
{
    if ((objPtr == NULL) || (sysMonObjectListsInit != RK_TRUE))
    {
        return;
    }

    RK_CR_AREA
    RK_CR_ENTER
    INT const family = kSysMonFamilyIndex_(objPtr->objID);
    if ((family >= 0) && (objPtr->sysMonListed == RK_TRUE) &&
        (objPtr->sysMonNode.nextPtr != NULL) &&
        (objPtr->sysMonNode.prevPtr != NULL))
    {
        (VOID)kListRemove(&sysMonObjectLists[family],
                          &objPtr->sysMonNode);
    }
    objPtr->sysMonNode.nextPtr = NULL;
    objPtr->sysMonNode.prevPtr = NULL;
    objPtr->sysMonListed = RK_FALSE;
    RK_CR_EXIT
}

RK_ERR kSysMonObjectNameSet(RK_HANDLE const objHandle,
                            CHAR const *const namePtr)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    if (namePtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    RK_KOBJ *objPtr = NULL;
    RK_ERR const resolveErr = kSysMonResolveObject_(objHandle, &objPtr);
    if (resolveErr != RK_ERR_SUCCESS)
    {
        return (resolveErr);
    }

    RK_CR_AREA
    RK_CR_ENTER
    if ((objPtr->init != RK_TRUE) ||
        (kSysMonFamilyIndex_(objPtr->objID) < 0))
    {
        RK_CR_EXIT
        return (RK_ERR_INVALID_OBJ);
    }

    kSysMonNameCopy_(objPtr->objName, RK_NAME_SIZE, namePtr);
#if (RK_CONF_MRM == ON)
    if (objPtr->objID == RK_MRM_KOBJ_ID)
    {
        RK_MRM *const mrmPtr = (RK_MRM *)objPtr;
        kSysMonNameCopyWithSuffix_(mrmPtr->mrmMem.objName, namePtr, 'B');
        kSysMonNameCopyWithSuffix_(mrmPtr->mrmDataMem.objName, namePtr, 'D');
    }
#endif
    RK_CR_EXIT
    return (RK_ERR_SUCCESS);
}

static VOID kSysMonPrintTasks_(VOID)
{
    UINT const count = kSysMonTaskCount_();

    printf("\r\ntasks total=%u\r\n", count);
    printf("tid name    st     pr nom runs     stack     ev-cur   ev-req   "
           "mux mode module\r\n");
    for (RK_TID tid = 0U; tid < RK_NTHREADS; tid++)
    {
        if (kSysMonSnapshotTask_(tid, &sysMonTaskRow) != RK_TRUE)
        {
            continue;
        }

        RK_SYSMON_TASK_ROW_ const *const rowPtr = &sysMonTaskRow;
        printf("%3u %-7s %-6s %2u %3u %-8lu %4u/%-4u 0x%06lx 0x%06lx "
               "%3lu %-4s %-7s\r\n",
               (UINT)rowPtr->tid, rowPtr->name,
               kSysMonStatusName_(rowPtr->status),
               (UINT)rowPtr->priority,
               (UINT)rowPtr->prioNominal,
               rowPtr->runCnt,
               (UINT)rowPtr->stackFreeWords,
               (UINT)rowPtr->stackSizeWords,
               rowPtr->eventCurr,
               rowPtr->eventReq,
               rowPtr->ownedMutexes,
               (rowPtr->privileged == RK_TRUE) ? "priv" : "user",
               rowPtr->moduleName);
    }
}

static VOID kSysMonPrintTruncation_(UINT const shown, UINT const total)
{
    if (shown < total)
    {
        printf("(shown %u of %u; raise RK_CONF_SYSMON_SNAPSHOT_MAX)\r\n",
               shown, total);
    }
}

static VOID kSysMonPrintObjects_(VOID)
{
    ULONG counts[RK_SYSMON_FAMILY_COUNT];

    kSysMonObjectListsEnsure_();
    RK_CR_AREA
    RK_CR_ENTER
    for (UINT i = 0U; i < (UINT)RK_SYSMON_FAMILY_COUNT; i++)
    {
        counts[i] = sysMonObjectLists[i].size;
    }
    RK_CR_EXIT

    printf("\r\nobjects\r\n");
    printf("type    count\r\n");
    for (UINT i = 0U; i < (UINT)RK_SYSMON_FAMILY_COUNT; i++)
    {
        RK_ID const objID = kSysMonFamilyObjId_((RK_SYSMON_FAMILY_)i);
        printf("%-7s %lu\r\n", kSysMonObjTypeName_(objID), counts[i]);
    }
}

static VOID kSysMonPrintObjectCommon_(RK_SYSMON_OBJECT_ROW_ const *const rowPtr)
{
    printf("%-7s %-7s 0x%08lx %-6s %-7s ",
           kSysMonObjTypeName_(rowPtr->objID),
           rowPtr->objName,
           (ULONG)(UINTPTR)rowPtr->objPtr,
           kSysMonScopeName_(rowPtr->scope),
           rowPtr->moduleName);
}

static VOID kSysMonPrintMem_(VOID)
{
    UINT total = 0U;
    UINT const count = kSysMonSnapshotObjects_(RK_MEMALLOC_KOBJ_ID, &total);

    printf("\r\nmemory partitions total=%u\r\n", total);
    printf("type    name    addr       scope  module  used/max blk wait\r\n");
    for (UINT i = 0U; i < count; i++)
    {
        RK_SYSMON_OBJECT_ROW_ const *const rowPtr = &sysMonObjectRows[i];
        kSysMonPrintObjectCommon_(rowPtr);
        printf("%lu/%lu %lu %lu\r\n",
               rowPtr->value0, rowPtr->value1, rowPtr->value2,
               rowPtr->value3);
    }
    kSysMonPrintTruncation_(count, total);
}

static VOID kSysMonPrintSema_(VOID)
{
#if (RK_CONF_SEMAPHORE == ON)
    UINT total = 0U;
    UINT const count = kSysMonSnapshotObjects_(RK_SEMAPHORE_KOBJ_ID, &total);

    printf("\r\nsemaphores total=%u\r\n", total);
    printf("type    name    addr       scope  module  value/max wait\r\n");
    for (UINT i = 0U; i < count; i++)
    {
        RK_SYSMON_OBJECT_ROW_ const *const rowPtr = &sysMonObjectRows[i];
        kSysMonPrintObjectCommon_(rowPtr);
        printf("%lu/%lu %lu\r\n",
               rowPtr->value0, rowPtr->value1, rowPtr->value2);
    }
    kSysMonPrintTruncation_(count, total);
#else
    printf("\r\nsemaphores disabled\r\n");
#endif
}

static VOID kSysMonPrintSleepq_(VOID)
{
#if (RK_CONF_SLEEP_QUEUE == ON)
    UINT total = 0U;
    UINT const count = kSysMonSnapshotObjects_(RK_SLEEPQ_KOBJ_ID, &total);

    printf("\r\nsleep queues total=%u\r\n", total);
    printf("type    name    addr       scope  module  wait\r\n");
    for (UINT i = 0U; i < count; i++)
    {
        RK_SYSMON_OBJECT_ROW_ const *const rowPtr = &sysMonObjectRows[i];
        kSysMonPrintObjectCommon_(rowPtr);
        printf("%lu\r\n", rowPtr->value0);
    }
    kSysMonPrintTruncation_(count, total);
#else
    printf("\r\nsleep queues disabled\r\n");
#endif
}

static VOID kSysMonPrintMutex_(VOID)
{
#if (RK_CONF_MUTEX == ON)
    UINT total = 0U;
    UINT const count = kSysMonSnapshotObjects_(RK_MUTEX_KOBJ_ID, &total);

    printf("\r\nmutexes total=%u\r\n", total);
    printf("type    name    addr       scope  module  lock proto wait fault "
           "owner\r\n");
    for (UINT i = 0U; i < count; i++)
    {
        RK_SYSMON_OBJECT_ROW_ const *const rowPtr = &sysMonObjectRows[i];
        kSysMonPrintObjectCommon_(rowPtr);
        printf("%lu %s %lu %lu ",
               rowPtr->value0,
               (rowPtr->value1 == RK_PRIO_INHERITANCE) ? "inherit" : "none",
               rowPtr->value2,
               rowPtr->value3);
        if (rowPtr->ownerValid == RK_TRUE)
        {
            printf("%s/%u\r\n", rowPtr->ownerName, (UINT)rowPtr->ownerTid);
        }
        else
        {
            printf("-\r\n");
        }
    }
    kSysMonPrintTruncation_(count, total);
#else
    printf("\r\nmutexes disabled\r\n");
#endif
}

static VOID kSysMonPrintQueues_(VOID)
{
#if (RK_CONF_MESG_QUEUE == ON)
    UINT total = 0U;
    UINT const count = kSysMonSnapshotObjects_(RK_MESGQQUEUE_KOBJ_ID, &total);

    printf("\r\nqueues total=%u\r\n", total);
    printf("type    name    addr       scope  module  used/max recv send bcast\r\n");
    for (UINT i = 0U; i < count; i++)
    {
        RK_SYSMON_OBJECT_ROW_ const *const rowPtr = &sysMonObjectRows[i];
        kSysMonPrintObjectCommon_(rowPtr);
        printf("%lu/%lu %lu %lu %lu\r\n",
               rowPtr->value0, rowPtr->value1, rowPtr->value2,
               rowPtr->value3, rowPtr->value4);
    }
    kSysMonPrintTruncation_(count, total);
#else
    printf("\r\nmessage queues disabled\r\n");
#endif
}

static VOID kSysMonPrintMrm_(VOID)
{
#if (RK_CONF_MRM == ON)
    UINT total = 0U;
    UINT const count = kSysMonSnapshotObjects_(RK_MRM_KOBJ_ID, &total);

    printf("\r\nmrm total=%u\r\n", total);
    printf("type    name    addr       scope  module  size active users\r\n");
    for (UINT i = 0U; i < count; i++)
    {
        RK_SYSMON_OBJECT_ROW_ const *const rowPtr = &sysMonObjectRows[i];
        kSysMonPrintObjectCommon_(rowPtr);
        printf("%lu %lu %lu\r\n",
               rowPtr->value0, rowPtr->value1, rowPtr->value2);
    }
    kSysMonPrintTruncation_(count, total);
#else
    printf("\r\nmrm disabled\r\n");
#endif
}

static VOID kSysMonPrintTimers_(VOID)
{
#if (RK_CONF_CALLOUT_TIMER == ON)
    UINT total = 0U;
    UINT const count = kSysMonSnapshotObjects_(RK_TIMER_KOBJ_ID, &total);

    printf("\r\ntimers total=%u\r\n", total);
    printf("type    name    addr       scope  module  armed reload period phase\r\n");
    for (UINT i = 0U; i < count; i++)
    {
        RK_SYSMON_OBJECT_ROW_ const *const rowPtr = &sysMonObjectRows[i];
        kSysMonPrintObjectCommon_(rowPtr);
        printf("%lu %lu %lu %lu\r\n",
               rowPtr->value0, rowPtr->value1, rowPtr->value2,
               rowPtr->value3);
    }
    kSysMonPrintTruncation_(count, total);
#else
    printf("\r\ntimers disabled\r\n");
#endif
}

static VOID kSysMonPrintShared_(VOID)
{
    UINT total = 0U;
    UINT const count = kSysMonSnapshotObjects_(RK_SHARED_MEM_KOBJ_ID, &total);

    printf("\r\nshared memory total=%u\r\n", total);
    printf("type    name    addr       scope  module  bytes attaches\r\n");
    for (UINT i = 0U; i < count; i++)
    {
        RK_SYSMON_OBJECT_ROW_ const *const rowPtr = &sysMonObjectRows[i];
        kSysMonPrintObjectCommon_(rowPtr);
        printf("%lu %lu\r\n", rowPtr->value0, rowPtr->value1);
    }
    kSysMonPrintTruncation_(count, total);
}

static VOID kSysMonPrintHelp_(VOID)
{
    printf("\r\ncommands\r\n");
    printf("  help\r\n");
    printf("  list tasks\r\n");
    printf("  list objects\r\n");
    printf("  list sema\r\n");
    printf("  list mutex\r\n");
    printf("  list queue\r\n");
    printf("  list timers\r\n");
    printf("  list mem\r\n");
    printf("  list sleepq\r\n");
    printf("  list mrm\r\n");
    printf("  list shared\r\n");
    printf("  exit\r\n");
}

static CHAR kSysMonLower_(CHAR const ch)
{
    if ((ch >= 'A') && (ch <= 'Z'))
    {
        return ((CHAR)(ch + ('a' - 'A')));
    }

    return (ch);
}

static CHAR const *kSysMonSkipSpaces_(CHAR const *linePtr)
{
    while ((linePtr != NULL) && (*linePtr == ' '))
    {
        linePtr++;
    }
    return (linePtr);
}

static RK_BOOL kSysMonStrEq_(CHAR const *lhsPtr, CHAR const *rhsPtr)
{
    lhsPtr = kSysMonSkipSpaces_(lhsPtr);
    rhsPtr = kSysMonSkipSpaces_(rhsPtr);
    if ((lhsPtr == NULL) || (rhsPtr == NULL))
    {
        return (RK_FALSE);
    }

    while ((*lhsPtr != '\0') && (*rhsPtr != '\0'))
    {
        if (kSysMonLower_(*lhsPtr) != kSysMonLower_(*rhsPtr))
        {
            return (RK_FALSE);
        }
        lhsPtr++;
        rhsPtr++;
    }

    return ((*lhsPtr == '\0') && (*rhsPtr == '\0')) ? RK_TRUE : RK_FALSE;
}

static VOID kSysMonTrimRight_(CHAR *const linePtr)
{
    UINT len = 0U;

    if (linePtr == NULL)
    {
        return;
    }

    while (linePtr[len] != '\0')
    {
        len++;
    }

    while ((len > 0U) && (linePtr[len - 1U] == ' '))
    {
        linePtr[len - 1U] = '\0';
        len--;
    }
}

static VOID kSysMonExec_(CHAR const *linePtr)
{
    linePtr = kSysMonSkipSpaces_(linePtr);
    if ((linePtr == NULL) || (*linePtr == '\0'))
    {
        return;
    }

    if ((kSysMonStrEq_(linePtr, "exit") == RK_TRUE) ||
        (kSysMonStrEq_(linePtr, "quit") == RK_TRUE))
    {
        printf("\r\nSysMon exit\r\n");
        sysMonDiagnosisActive = RK_FALSE;
        kLogNormalOutputSet(sysMonSavedLogNormalOutput);
    }
    else if ((kSysMonStrEq_(linePtr, "help") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "?") == RK_TRUE))
    {
        kSysMonPrintHelp_();
    }
    else if ((kSysMonStrEq_(linePtr, "list tasks") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "tasks") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "ps") == RK_TRUE))
    {
        kSysMonPrintTasks_();
    }
    else if ((kSysMonStrEq_(linePtr, "list objects") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "objects") == RK_TRUE))
    {
        kSysMonPrintObjects_();
    }
    else if ((kSysMonStrEq_(linePtr, "list sema") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "list semas") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "list semaphore") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "list semaphores") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "sema") == RK_TRUE))
    {
        kSysMonPrintSema_();
    }
    else if ((kSysMonStrEq_(linePtr, "list mutex") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "list mutexes") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "mutex") == RK_TRUE))
    {
        kSysMonPrintMutex_();
    }
    else if ((kSysMonStrEq_(linePtr, "list queue") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "list queues") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "list mesg") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "queue") == RK_TRUE))
    {
        kSysMonPrintQueues_();
    }
    else if ((kSysMonStrEq_(linePtr, "list timers") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "list timer") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "timers") == RK_TRUE))
    {
        kSysMonPrintTimers_();
    }
    else if ((kSysMonStrEq_(linePtr, "list mem") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "list memory") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "mem") == RK_TRUE))
    {
        kSysMonPrintMem_();
    }
    else if ((kSysMonStrEq_(linePtr, "list sleepq") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "sleepq") == RK_TRUE))
    {
        kSysMonPrintSleepq_();
    }
    else if ((kSysMonStrEq_(linePtr, "list mrm") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "mrm") == RK_TRUE))
    {
        kSysMonPrintMrm_();
    }
    else if ((kSysMonStrEq_(linePtr, "list shared") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "list shm") == RK_TRUE) ||
             (kSysMonStrEq_(linePtr, "shared") == RK_TRUE))
    {
        kSysMonPrintShared_();
    }
    else
    {
        printf("\r\nunknown command: %s\r\n", linePtr);
        kSysMonPrintHelp_();
    }
}

static VOID kSysMonPrompt_(VOID)
{
    printf("\r\nrk> ");
}

static VOID kSysMonEnterDiagnosis_(VOID)
{
    if (sysMonDiagnosisActive == RK_TRUE)
    {
        return;
    }

    sysMonSavedLogNormalOutput = kLogNormalOutputGet();
    kLogNormalOutputSet(RK_FALSE);
    sysMonDiagnosisActive = RK_TRUE;
    kSysMonPrompt_();
}

static VOID kSysMonSubmitLine_(VOID)
{
    sysMonLine[sysMonLineLen] = '\0';
    kSysMonTrimRight_(sysMonLine);
    kSysMonExec_(sysMonLine);
    sysMonLineLen = 0U;
    if (sysMonDiagnosisActive == RK_TRUE)
    {
        kSysMonPrompt_();
    }
}

VOID kSysMonPoll(VOID)
{
    CHAR ch;

    while (kConsoleGetc(&ch) > 0)
    {
        if (sysMonDiagnosisActive == RK_FALSE)
        {
            if ((ch != '\r') && (ch != '\n') &&
                ((ch < ' ') || (ch > '~')))
            {
                continue;
            }
            kSysMonEnterDiagnosis_();
        }

        if ((ch == '\n') && (sysMonDropLf == RK_TRUE))
        {
            sysMonDropLf = RK_FALSE;
            continue;
        }
        sysMonDropLf = RK_FALSE;

        if ((ch == '\r') || (ch == '\n'))
        {
            if (ch == '\r')
            {
                sysMonDropLf = RK_TRUE;
            }
            kPutc('\r');
            kPutc('\n');
            kSysMonSubmitLine_();
        }
        else if ((ch == '\b') || (ch == 0x7F))
        {
            if (sysMonLineLen > 0U)
            {
                sysMonLineLen--;
                kPuts("\b \b");
            }
        }
        else if ((ch >= ' ') && (ch <= '~'))
        {
            if (sysMonLineLen < (RK_CONF_SYSMON_LINE_LEN - 1U))
            {
                sysMonLine[sysMonLineLen] = ch;
                sysMonLineLen++;
                kPutc(ch);
            }
        }
        else
        {
            /* Ignore terminal control bytes outside the supported line editor. */
        }
    }
}

static VOID kSysMonTask_(VOID *args)
{
    K_UNUSE(args);

    while (1)
    {
        kSysMonPoll();
        (VOID)kSleepDelay((RK_TICK)RK_CONF_SYSMON_POLL_TICKS);
    }
}

RK_ERR kSysMonInit(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    if (sysMonTaskHandle != NULL)
    {
        return (RK_ERR_SUCCESS);
    }

    kSysMonObjectListsEnsure_();
    kBoardConsoleInit();

    return (kTaskInitPrivileged(&sysMonTaskHandle, kSysMonTask_, RK_NO_ARGS,
                                "SysMon", sysMonStack,
                                RK_CONF_SYSMON_STACKSIZE,
                                RK_CONF_SYSMON_PRIO, RK_PREEMPT));
}

#endif /* RK_CONF_SYSMON */
