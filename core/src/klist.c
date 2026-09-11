/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.2.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#include <kenv.h>
#include <kcoredefs.h>
#include <kcommondefs.h>
#include <kobjs.h>
#include <klist.h>

/*
 * File intent:
 *   Intrusive doubly-linked list primitive used by scheduler queues, wait
 *   lists and object ownership chains. Nodes live inside the objects they link.
 */

/******************************************************************************/
/* GENERIC DOUBLY LIST                                                        */
/******************************************************************************/

/*
 * Intrusive-list contract:
 *   - listDummy is a permanent sentinel; it is never removed.
 *   - size counts only real nodes.
 *   - A linked node's prev/next pointers are owned by exactly one list.
 */
RK_ERR kListInit(RK_LIST *const kobj)
{
    K_ASSERT(kobj != NULL);
    kobj->listDummy.nextPtr = &(kobj->listDummy);
    kobj->listDummy.prevPtr = &(kobj->listDummy);
    kobj->size = 0U;
    return (RK_ERR_SUCCESS);
}

RK_ERR kListInsertAfter(RK_LIST *const kobj, RK_NODE *const refNodePtr,
                        RK_NODE *const newNodePtr)
{
    K_ASSERT(kobj != NULL && newNodePtr != NULL && refNodePtr != NULL);
    newNodePtr->nextPtr = refNodePtr->nextPtr;
    refNodePtr->nextPtr->prevPtr = newNodePtr;
    newNodePtr->prevPtr = refNodePtr;
    refNodePtr->nextPtr = newNodePtr;
    kobj->size += 1U;
    return (RK_ERR_SUCCESS);
}

RK_ERR kListRemove(RK_LIST *const kobj, RK_NODE *const remNodePtr)
{
    K_ASSERT(kobj != NULL && remNodePtr != NULL);
    K_ASSERT(remNodePtr != &(kobj->listDummy));
    if (kobj->size == 0)
    {
        return (RK_ERR_LIST_EMPTY);
    }
    KLISTNODEDEL(remNodePtr);
    kobj->size -= 1U;
    return (RK_ERR_SUCCESS);
}

RK_ERR kListRemoveHead(RK_LIST *const kobj, RK_NODE **const remNodePPtr)
{
    K_ASSERT(kobj != NULL && remNodePPtr != NULL);
    if (kobj->size == 0)
    {
        return (RK_ERR_LIST_EMPTY);
    }

    RK_NODE *currHeadPtr = kobj->listDummy.nextPtr;
    *remNodePPtr = currHeadPtr;
    KLISTNODEDEL(currHeadPtr);
    kobj->size -= 1U;
    return (RK_ERR_SUCCESS);
}

RK_ERR kListAddTail(RK_LIST *const kobj, RK_NODE *const newNodePtr)
{
    return (kListInsertAfter(kobj, kobj->listDummy.prevPtr, newNodePtr));
}

RK_ERR kListAddHead(RK_LIST *const kobj, RK_NODE *const newNodePtr)
{
    return (kListInsertAfter(kobj, &kobj->listDummy, newNodePtr));
}

RK_ERR kListRemoveTail(RK_LIST *const kobj, RK_NODE **remNodePPtr)
{
    K_ASSERT(kobj != NULL && remNodePPtr != NULL);
    if (kobj->size == 0)
    {
        return (RK_ERR_LIST_EMPTY);
    }

    RK_NODE *currTailPtr = kobj->listDummy.prevPtr;
    K_ASSERT(currTailPtr != NULL);
    *remNodePPtr = currTailPtr;
    KLISTNODEDEL(currTailPtr);
    kobj->size -= 1U;
    return (RK_ERR_SUCCESS);
}
