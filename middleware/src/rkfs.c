/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                   */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

#include <rkfs.h>
#include <klogger.h>
#include <kstring.h>

#if defined(RK_MCU_F401RE)
extern BYTE __rk_fs_flash_begin;
extern BYTE __rk_fs_flash_end;
#define RKFS_FLASH_BASE ((UINTPTR)&__rk_fs_flash_begin)
#define RKFS_FLASH_END ((UINTPTR)&__rk_fs_flash_end)
#define STM32_FLASH_KEYR (*(volatile ULONG *)0x40023C04UL)
#define STM32_FLASH_SR (*(volatile ULONG *)0x40023C0CUL)
#define STM32_FLASH_CR (*(volatile ULONG *)0x40023C10UL)
#define STM32_FLASH_KEY1 (0x45670123UL)
#define STM32_FLASH_KEY2 (0xCDEF89ABUL)
#define STM32_FLASH_SR_BSY (1UL << 16U)
#define STM32_FLASH_SR_ERR_MASK                                              \
    ((1UL << 7U) | (1UL << 6U) | (1UL << 5U) | (1UL << 4U) | (1UL << 1U))
#define STM32_FLASH_CR_LOCK (1UL << 31U)
#define STM32_FLASH_CR_STRT (1UL << 16U)
#define STM32_FLASH_CR_PSIZE_WORD (2UL << 8U)
#define STM32_FLASH_CR_SER (1UL << 1U)
#define STM32_FLASH_CR_PG (1UL << 0U)
#define STM32_FLASH_CR_SNB_SHIFT (3U)
#define RKFS_FIRST_SECTOR (6UL)
#else
#define RKFS_FLASH_BASE (0UL)
#define RKFS_FLASH_END (0UL)
#define STM32_FLASH_KEYR (*(volatile ULONG *)0UL)
#define STM32_FLASH_SR (*(volatile ULONG *)0UL)
#define STM32_FLASH_CR (*(volatile ULONG *)0UL)
#define STM32_FLASH_KEY1 (0UL)
#define STM32_FLASH_KEY2 (0UL)
#define STM32_FLASH_SR_BSY (0UL)
#define STM32_FLASH_SR_ERR_MASK (0UL)
#define STM32_FLASH_CR_LOCK (0UL)
#define STM32_FLASH_CR_STRT (0UL)
#define STM32_FLASH_CR_PSIZE_WORD (0UL)
#define STM32_FLASH_CR_SER (0UL)
#define STM32_FLASH_CR_PG (0UL)
#define STM32_FLASH_CR_SNB_SHIFT (0U)
#define RKFS_FIRST_SECTOR (0UL)
#endif

static VOID rkFsCopyText_(CHAR *const dstPtr,
                          ULONG const dstBytes,
                          CHAR const *const srcPtr)
{
    ULONG i;

    for (i = 0UL; i < dstBytes; i++)
    {
        dstPtr[i] = '\0';
    }

    if (srcPtr == NULL)
    {
        return;
    }

    for (i = 0UL; (i + 1UL) < dstBytes; i++)
    {
        if (srcPtr[i] == '\0')
        {
            break;
        }
        dstPtr[i] = srcPtr[i];
    }
}

static ULONG rkFsTextLen_(CHAR const *const textPtr, ULONG const maxBytes)
{
    ULONG i;

    for (i = 0UL; i < maxBytes; i++)
    {
        if (textPtr[i] == '\0')
        {
            return (i);
        }
    }

    return (maxBytes);
}

static RKFS_STATUS rkFsStatusFromLfs_(INT const err)
{
    switch (err)
    {
        case LFS_ERR_OK:
            return (RKFS_STATUS_OK);
        case LFS_ERR_NOENT:
            return (RKFS_STATUS_NOT_FOUND);
        case LFS_ERR_EXIST:
            return (RKFS_STATUS_EXISTS);
        case LFS_ERR_NOSPC:
            return (RKFS_STATUS_NO_SPACE);
        case LFS_ERR_INVAL:
        case LFS_ERR_NAMETOOLONG:
        case LFS_ERR_NOTDIR:
        case LFS_ERR_ISDIR:
            return (RKFS_STATUS_INVALID);
        default:
            return (RKFS_STATUS_IO);
    }
}

static VOID rkFsFlashWait_(VOID)
{
    while ((STM32_FLASH_SR & STM32_FLASH_SR_BSY) != 0UL)
    {
    }
}

static VOID rkFsFlashUnlock_(VOID)
{
    if ((STM32_FLASH_CR & STM32_FLASH_CR_LOCK) != 0UL)
    {
        STM32_FLASH_KEYR = STM32_FLASH_KEY1;
        STM32_FLASH_KEYR = STM32_FLASH_KEY2;
    }
}

static INT rkFsFlashClearErr_(VOID)
{
    rkFsFlashWait_();
    STM32_FLASH_SR = STM32_FLASH_SR_ERR_MASK;
    return (((STM32_FLASH_SR & STM32_FLASH_SR_ERR_MASK) == 0UL)
                ? LFS_ERR_OK
                : LFS_ERR_IO);
}

static UINTPTR rkFsBlockAddr_(lfs_block_t const block)
{
    return (RKFS_FLASH_BASE + ((UINTPTR)block * (UINTPTR)RKFS_BLOCK_SIZE));
}

static INT rkFsBdRead_(struct lfs_config const *const cfgPtr,
                       lfs_block_t const block,
                       lfs_off_t const off,
                       VOID *const bufferPtr,
                       lfs_size_t const size)
{
    UINTPTR const addr = rkFsBlockAddr_(block) + (UINTPTR)off;

    (VOID)cfgPtr;

    if ((block >= RKFS_BLOCK_COUNT) ||
        ((off + size) > RKFS_BLOCK_SIZE) ||
        ((addr + (UINTPTR)size) > RKFS_FLASH_END))
    {
        return (LFS_ERR_IO);
    }

    RK_MEMCPY(bufferPtr, (VOID const *)addr, size);
    return (LFS_ERR_OK);
}

static INT rkFsBdProg_(struct lfs_config const *const cfgPtr,
                       lfs_block_t const block,
                       lfs_off_t const off,
                       VOID const *const bufferPtr,
                       lfs_size_t const size)
{
    UINTPTR const addr = rkFsBlockAddr_(block) + (UINTPTR)off;
    BYTE const *const srcPtr = (BYTE const *)bufferPtr;
    lfs_size_t i;

    (VOID)cfgPtr;

    if ((block >= RKFS_BLOCK_COUNT) ||
        ((off + size) > RKFS_BLOCK_SIZE) ||
        ((addr + (UINTPTR)size) > RKFS_FLASH_END) ||
        ((addr & 3UL) != 0UL) ||
        ((size & 3U) != 0U))
    {
        return (LFS_ERR_IO);
    }

    rkFsFlashUnlock_();
    if (rkFsFlashClearErr_() != LFS_ERR_OK)
    {
        return (LFS_ERR_IO);
    }

    STM32_FLASH_CR = STM32_FLASH_CR_PSIZE_WORD | STM32_FLASH_CR_PG;
    for (i = 0U; i < size; i += 4U)
    {
        ULONG word;

        RK_MEMCPY(&word, &srcPtr[i], sizeof(word));
        *(volatile ULONG *)(addr + (UINTPTR)i) = word;
        rkFsFlashWait_();
        if ((STM32_FLASH_SR & STM32_FLASH_SR_ERR_MASK) != 0UL)
        {
            STM32_FLASH_CR = 0UL;
            return (LFS_ERR_IO);
        }
    }
    STM32_FLASH_CR = 0UL;

    return (LFS_ERR_OK);
}

static INT rkFsBdErase_(struct lfs_config const *const cfgPtr,
                        lfs_block_t const block)
{
    ULONG const sector = RKFS_FIRST_SECTOR + (ULONG)block;

    (VOID)cfgPtr;

    if ((block >= RKFS_BLOCK_COUNT) ||
        ((rkFsBlockAddr_(block) + RKFS_BLOCK_SIZE) > RKFS_FLASH_END))
    {
        return (LFS_ERR_IO);
    }

    rkFsFlashUnlock_();
    if (rkFsFlashClearErr_() != LFS_ERR_OK)
    {
        return (LFS_ERR_IO);
    }

    STM32_FLASH_CR = STM32_FLASH_CR_PSIZE_WORD |
                     (sector << STM32_FLASH_CR_SNB_SHIFT) |
                     STM32_FLASH_CR_SER;
    STM32_FLASH_CR |= STM32_FLASH_CR_STRT;
    rkFsFlashWait_();

    if ((STM32_FLASH_SR & STM32_FLASH_SR_ERR_MASK) != 0UL)
    {
        STM32_FLASH_CR = 0UL;
        return (LFS_ERR_IO);
    }

    STM32_FLASH_CR = 0UL;
    return (LFS_ERR_OK);
}

static INT rkFsBdSync_(struct lfs_config const *const cfgPtr)
{
    (VOID)cfgPtr;
    rkFsFlashWait_();
    return (LFS_ERR_OK);
}

static VOID rkFsConfig_(RKFS_RAM *const fsPtr)
{
    RK_MEMSET(&fsPtr->lfs, 0, sizeof(fsPtr->lfs));
    RK_MEMSET(&fsPtr->cfg, 0, sizeof(fsPtr->cfg));
    RK_MEMSET(fsPtr->readCache, 0, sizeof(fsPtr->readCache));
    RK_MEMSET(fsPtr->progCache, 0, sizeof(fsPtr->progCache));
    RK_MEMSET(fsPtr->fileCache, 0, sizeof(fsPtr->fileCache));
    RK_MEMSET(fsPtr->lookahead, 0, sizeof(fsPtr->lookahead));

    fsPtr->cfg.context = fsPtr;
    fsPtr->cfg.read = rkFsBdRead_;
    fsPtr->cfg.prog = rkFsBdProg_;
    fsPtr->cfg.erase = rkFsBdErase_;
    fsPtr->cfg.sync = rkFsBdSync_;
    fsPtr->cfg.read_size = 16U;
    fsPtr->cfg.prog_size = 16U;
    fsPtr->cfg.block_size = RKFS_BLOCK_SIZE;
    fsPtr->cfg.block_count = RKFS_BLOCK_COUNT;
    fsPtr->cfg.block_cycles = 100;
    fsPtr->cfg.cache_size = RKFS_CACHE_BYTES;
    fsPtr->cfg.lookahead_size = RKFS_LOOKAHEAD_BYTES;
    fsPtr->cfg.read_buffer = fsPtr->readCache;
    fsPtr->cfg.prog_buffer = fsPtr->progCache;
    fsPtr->cfg.lookahead_buffer = fsPtr->lookahead;
}

static RKFS_STATUS rkFsMount_(RKFS_RAM *const fsPtr)
{
    INT err;

    rkFsConfig_(fsPtr);
    kLog("FS LittleFS mount start: flash base=0x%lx bytes=%lu",
         (ULONG)RKFS_FLASH_BASE,
         (ULONG)(RKFS_FLASH_END - RKFS_FLASH_BASE));

    err = lfs_mount(&fsPtr->lfs, &fsPtr->cfg);
    if (err == LFS_ERR_OK)
    {
        return (RKFS_STATUS_OK);
    }

    kLog("FS LittleFS mount status=%ld; formatting reserved flash",
         (LONG)err);

    err = lfs_format(&fsPtr->lfs, &fsPtr->cfg);
    if (err < 0)
    {
        kLog("FS LittleFS format failed status=%ld", (LONG)err);
        return (rkFsStatusFromLfs_(err));
    }

    err = lfs_mount(&fsPtr->lfs, &fsPtr->cfg);
    if (err < 0)
    {
        kLog("FS LittleFS mount after format failed status=%ld", (LONG)err);
    }
    return ((err < 0) ? rkFsStatusFromLfs_(err) : RKFS_STATUS_OK);
}

static VOID rkFsMkdir_(RKFS_RAM *const fsPtr,
                       RKFS_REQUEST const *const reqPtr,
                       RKFS_REPLY *const replyPtr)
{
    INT const err = lfs_mkdir(&fsPtr->lfs, reqPtr->path);

    replyPtr->status = rkFsStatusFromLfs_(err);
    rkFsCopyText_(replyPtr->path, RKFS_PATH_BYTES, reqPtr->path);
}

static VOID rkFsWriteRecord_(RKFS_RAM *const fsPtr,
                             RKFS_REQUEST const *const reqPtr,
                             RKFS_REPLY *const replyPtr)
{
    lfs_file_t file;
    struct lfs_file_config fileCfg;
    INT err;
    lfs_ssize_t written;
    ULONG const bytes = rkFsTextLen_(reqPtr->data, RKFS_RECORD_BYTES);

    RK_MEMSET(&file, 0, sizeof(file));
    RK_MEMSET(&fileCfg, 0, sizeof(fileCfg));
    fileCfg.buffer = fsPtr->fileCache;

    err = lfs_file_opencfg(&fsPtr->lfs, &file, reqPtr->path,
                           LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                           &fileCfg);
    if (err < 0)
    {
        replyPtr->status = rkFsStatusFromLfs_(err);
        return;
    }

    written = lfs_file_write(&fsPtr->lfs, &file, reqPtr->data,
                             (lfs_size_t)bytes);
    if (written < 0)
    {
        replyPtr->status = rkFsStatusFromLfs_((INT)written);
        (VOID)lfs_file_close(&fsPtr->lfs, &file);
        return;
    }

    err = lfs_file_close(&fsPtr->lfs, &file);
    replyPtr->status = rkFsStatusFromLfs_(err);
    replyPtr->bytes = (ULONG)written;
    rkFsCopyText_(replyPtr->path, RKFS_PATH_BYTES, reqPtr->path);
    rkFsCopyText_(replyPtr->data, RKFS_RECORD_BYTES, reqPtr->data);
}

static VOID rkFsReadRecord_(RKFS_RAM *const fsPtr,
                            RKFS_REQUEST const *const reqPtr,
                            RKFS_REPLY *const replyPtr)
{
    lfs_file_t file;
    struct lfs_file_config fileCfg;
    INT err;
    lfs_ssize_t readBytes;

    RK_MEMSET(&file, 0, sizeof(file));
    RK_MEMSET(&fileCfg, 0, sizeof(fileCfg));
    fileCfg.buffer = fsPtr->fileCache;

    err = lfs_file_opencfg(&fsPtr->lfs, &file, reqPtr->path, LFS_O_RDONLY,
                           &fileCfg);
    if (err < 0)
    {
        replyPtr->status = rkFsStatusFromLfs_(err);
        return;
    }

    readBytes = lfs_file_read(&fsPtr->lfs, &file, replyPtr->data,
                              RKFS_RECORD_BYTES - 1U);
    if (readBytes < 0)
    {
        replyPtr->status = rkFsStatusFromLfs_((INT)readBytes);
        (VOID)lfs_file_close(&fsPtr->lfs, &file);
        return;
    }

    replyPtr->data[(ULONG)readBytes] = '\0';
    err = lfs_file_close(&fsPtr->lfs, &file);
    replyPtr->status = rkFsStatusFromLfs_(err);
    replyPtr->bytes = (ULONG)readBytes;
    rkFsCopyText_(replyPtr->path, RKFS_PATH_BYTES, reqPtr->path);
}

static VOID rkFsListDir_(RKFS_RAM *const fsPtr,
                         RKFS_REQUEST const *const reqPtr,
                         RKFS_REPLY *const replyPtr)
{
    lfs_dir_t dir;
    struct lfs_info info;
    INT err;

    RK_MEMSET(&dir, 0, sizeof(dir));
    RK_MEMSET(&info, 0, sizeof(info));

    err = lfs_dir_open(&fsPtr->lfs, &dir, reqPtr->path);
    if (err < 0)
    {
        replyPtr->status = rkFsStatusFromLfs_(err);
        return;
    }

    while (1)
    {
        err = lfs_dir_read(&fsPtr->lfs, &dir, &info);
        if (err < 0)
        {
            replyPtr->status = rkFsStatusFromLfs_(err);
            (VOID)lfs_dir_close(&fsPtr->lfs, &dir);
            return;
        }
        if (err == 0)
        {
            break;
        }
        if ((info.name[0] == '.') &&
            ((info.name[1] == '\0') ||
             ((info.name[1] == '.') && (info.name[2] == '\0'))))
        {
            continue;
        }
        replyPtr->count++;
    }

    err = lfs_dir_close(&fsPtr->lfs, &dir);
    replyPtr->status = rkFsStatusFromLfs_(err);
    rkFsCopyText_(replyPtr->path, RKFS_PATH_BYTES, reqPtr->path);
}

static VOID rkFsHandle_(RKFS_RAM *const fsPtr,
                        RKFS_REQUEST const *const reqPtr,
                        RKFS_REPLY *const replyPtr)
{
    RK_MEMSET(replyPtr, 0, sizeof(*replyPtr));

    switch ((RKFS_COMMAND)reqPtr->command)
    {
        case RKFS_CMD_MKDIR:
            rkFsMkdir_(fsPtr, reqPtr, replyPtr);
            break;
        case RKFS_CMD_WRITE_RECORD:
            rkFsWriteRecord_(fsPtr, reqPtr, replyPtr);
            break;
        case RKFS_CMD_READ_RECORD:
            rkFsReadRecord_(fsPtr, reqPtr, replyPtr);
            break;
        case RKFS_CMD_LIST_DIR:
            rkFsListDir_(fsPtr, reqPtr, replyPtr);
            break;
        default:
            replyPtr->status = RKFS_STATUS_INVALID;
            break;
    }
}

static RK_ERR rkFsCall_(RK_TASK_HANDLE const serverHandle,
                        RKFS_REQUEST *const reqPtr,
                        RKFS_REPLY *const replyPtr)
{
    RK_SYNCH_ATTR attr;
    ULONG replyBytes = 0UL;
    RK_ERR err;

    attr.reqPtr = reqPtr;
    attr.reqBytes = sizeof(*reqPtr);
    attr.replyPtr = replyPtr;
    attr.replyMaxBytes = sizeof(*replyPtr);
    attr.replyBytesPtr = &replyBytes;

    err = kSynchMesgCall(serverHandle, &attr, RK_WAIT_FOREVER);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }
    if (replyBytes != sizeof(*replyPtr))
    {
        return (RK_ERR_INVALID_MSG_SIZE);
    }

    return (RK_ERR_SUCCESS);
}

RK_ERR rkFsClientMkdir(RK_TASK_HANDLE const serverHandle,
                       CHAR const *const pathPtr)
{
    RKFS_REQUEST req;
    RKFS_REPLY reply;
    RK_ERR err;

    RK_MEMSET(&req, 0, sizeof(req));
    req.command = RKFS_CMD_MKDIR;
    rkFsCopyText_(req.path, RKFS_PATH_BYTES, pathPtr);

    err = rkFsCall_(serverHandle, &req, &reply);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    return ((reply.status == RKFS_STATUS_OK) ||
            (reply.status == RKFS_STATUS_EXISTS))
               ? RK_ERR_SUCCESS
               : RK_ERR_ERROR;
}

RK_ERR rkFsClientWriteRecord(RK_TASK_HANDLE const serverHandle,
                             CHAR const *const pathPtr,
                             CHAR const *const dataPtr)
{
    RKFS_REQUEST req;
    RKFS_REPLY reply;
    RK_ERR err;

    RK_MEMSET(&req, 0, sizeof(req));
    req.command = RKFS_CMD_WRITE_RECORD;
    rkFsCopyText_(req.path, RKFS_PATH_BYTES, pathPtr);
    rkFsCopyText_(req.data, RKFS_RECORD_BYTES, dataPtr);

    err = rkFsCall_(serverHandle, &req, &reply);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    return ((reply.status == RKFS_STATUS_OK) ? RK_ERR_SUCCESS : RK_ERR_ERROR);
}

RK_ERR rkFsClientReadRecord(RK_TASK_HANDLE const serverHandle,
                            CHAR const *const pathPtr,
                            RKFS_REPLY *const replyPtr)
{
    RKFS_REQUEST req;

    if (replyPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    RK_MEMSET(&req, 0, sizeof(req));
    req.command = RKFS_CMD_READ_RECORD;
    rkFsCopyText_(req.path, RKFS_PATH_BYTES, pathPtr);

    return (rkFsCall_(serverHandle, &req, replyPtr));
}

RK_ERR rkFsClientListDir(RK_TASK_HANDLE const serverHandle,
                         CHAR const *const pathPtr,
                         RKFS_REPLY *const replyPtr)
{
    RKFS_REQUEST req;

    if (replyPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    RK_MEMSET(&req, 0, sizeof(req));
    req.command = RKFS_CMD_LIST_DIR;
    rkFsCopyText_(req.path, RKFS_PATH_BYTES, pathPtr);

    return (rkFsCall_(serverHandle, &req, replyPtr));
}

VOID rkFsServerTask(VOID *args)
{
    RKFS_RAM *const fsPtr = (RKFS_RAM *)args;
    RKFS_STATUS const mountStatus = rkFsMount_(fsPtr);

    if (mountStatus != RKFS_STATUS_OK)
    {
        kLog("FS server stopped: mount status=%lu", (ULONG)mountStatus);
        while (1)
        {
            kSleep(RK_MS_TO_TICKS(1000UL));
        }
    }
    kLog("FS server ready: LittleFS mounted on reserved STM32 flash");

    while (1)
    {
        RK_SYNCH_CALL_DATA call;
        RKFS_REQUEST req;
        RKFS_REPLY reply;
        ULONG reqBytes = 0UL;

        if (kSynchMesgAccept(&call, &req, &reqBytes,
                             RK_WAIT_FOREVER) != RK_ERR_SUCCESS)
        {
            continue;
        }
        if (reqBytes != sizeof(req))
        {
            RK_MEMSET(&reply, 0, sizeof(reply));
            reply.status = RKFS_STATUS_INVALID;
        }
        else
        {
            rkFsHandle_(fsPtr, &req, &reply);
        }

        kSynchMesgReply(&call, &reply, sizeof(reply));

        kLog("FS LittleFS command=%lu path=%s status=%lu bytes=%lu count=%lu",
             req.command, req.path, reply.status, reply.bytes, reply.count);
    }
}
