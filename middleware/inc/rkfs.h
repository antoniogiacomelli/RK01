/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#ifndef RKFS_H
#define RKFS_H

#include <kapi_app.h>
#include <lfs.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define RKFS_SERVICE_RAM_BYTES (4096U)
#define RKFS_DOMAIN_BYTES RKFS_SERVICE_RAM_BYTES
#define RKFS_STACK_WORDS (1024U)
#define RKFS_PATH_BYTES (32U)
#define RKFS_RECORD_BYTES (64U)
#define RKFS_BLOCK_SIZE (131072U)
#define RKFS_BLOCK_COUNT (2U)
#define RKFS_CACHE_BYTES (256U)
#define RKFS_LOOKAHEAD_BYTES (8U)

typedef enum
{
    RKFS_CMD_MKDIR = 1,
    RKFS_CMD_WRITE_RECORD = 2,
    RKFS_CMD_READ_RECORD = 3,
    RKFS_CMD_LIST_DIR = 4
} RKFS_COMMAND;

typedef enum
{
    RKFS_STATUS_OK = 0,
    RKFS_STATUS_NOT_FOUND = 1,
    RKFS_STATUS_EXISTS = 2,
    RKFS_STATUS_NO_SPACE = 3,
    RKFS_STATUS_INVALID = 4,
    RKFS_STATUS_IO = 5
} RKFS_STATUS;

typedef struct
{
    ULONG command;
    CHAR path[RKFS_PATH_BYTES];
    CHAR data[RKFS_RECORD_BYTES];
} RKFS_REQUEST;

typedef struct
{
    ULONG status;
    ULONG bytes;
    ULONG count;
    CHAR path[RKFS_PATH_BYTES];
    CHAR data[RKFS_RECORD_BYTES];
} RKFS_REPLY;

typedef struct
{
    lfs_t lfs;
    struct lfs_config cfg;
    BYTE readCache[RKFS_CACHE_BYTES];
    BYTE progCache[RKFS_CACHE_BYTES];
    BYTE fileCache[RKFS_CACHE_BYTES];
    BYTE lookahead[RKFS_LOOKAHEAD_BYTES];
    BYTE reserved[RKFS_SERVICE_RAM_BYTES -
                  (sizeof(lfs_t) +
                   sizeof(struct lfs_config) +
                   (3U * RKFS_CACHE_BYTES) +
                   RKFS_LOOKAHEAD_BYTES)];
} RKFS_RAM;

_Static_assert(sizeof(RKFS_RAM) == RKFS_SERVICE_RAM_BYTES,
               "filesystem service RAM layout must fill its reserved region");

RK_ERR rkFsClientMkdir(RK_TASK_HANDLE serverHandle,
                       CHAR const *pathPtr);
RK_ERR rkFsClientWriteRecord(RK_TASK_HANDLE serverHandle,
                             CHAR const *pathPtr,
                             CHAR const *dataPtr);
RK_ERR rkFsClientReadRecord(RK_TASK_HANDLE serverHandle,
                            CHAR const *pathPtr,
                            RKFS_REPLY *replyPtr);
RK_ERR rkFsClientListDir(RK_TASK_HANDLE serverHandle,
                         CHAR const *pathPtr,
                         RKFS_REPLY *replyPtr);
VOID rkFsServerTask(VOID *args);

#ifdef __cplusplus
}
#endif

#endif
