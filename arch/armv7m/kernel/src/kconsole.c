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
 *   ARMv7-M board-console backend. STM32F401RE uses USART2; MPS2 AN386 uses
 *   CMSDK UART0 routed by QEMU to the first serial chardev.
 */

#define RK_SOURCE_CODE

#include <kconsole.h>
#include <kconfig.h>
#include <kcoredefs.h>
#include <ksch.h>
#include <ksyscall.h>
#include <ksynchmesg.h>
#include <ktaskevents.h>

#if defined(STM32F401xE)
#define RK_BOARD_CONSOLE_HAS_USART2 (1U)
#define RK_BOARD_CONSOLE_BAUD (115200UL)
#endif

#if defined(RK_MCU_MPS2_AN386)
#define RK_BOARD_CONSOLE_HAS_CMSDK_UART (1U)

#define MPS2_AN386_UART0_BASE (0x40004000UL)
#define CMSDK_UART_DATA                                                      \
    (*(volatile unsigned long *)(MPS2_AN386_UART0_BASE + 0x00UL))
#define CMSDK_UART_STATE                                                     \
    (*(volatile unsigned long *)(MPS2_AN386_UART0_BASE + 0x04UL))
#define CMSDK_UART_CTRL                                                      \
    (*(volatile unsigned long *)(MPS2_AN386_UART0_BASE + 0x08UL))
#define CMSDK_UART_INTSTATUS                                                 \
    (*(volatile unsigned long *)(MPS2_AN386_UART0_BASE + 0x0CUL))
#define CMSDK_UART_BAUDDIV                                                   \
    (*(volatile unsigned long *)(MPS2_AN386_UART0_BASE + 0x10UL))

#define CMSDK_UART_STATE_TXBF (1UL << 0U)
#define CMSDK_UART_STATE_RXBF (1UL << 1U)
#define CMSDK_UART_CTRL_TXEN (1UL << 0U)
#define CMSDK_UART_CTRL_RXEN (1UL << 1U)
#define CMSDK_UART_CTRL_RXIRQEN (1UL << 3U)
#define CMSDK_UART_INTSTATUS_RXIRQ (1UL << 1U)
#define CMSDK_UART_BAUD (115200UL)
#define CMSDK_UART0_IRQN (0UL)
#endif

#if defined(RK_BOARD_CONSOLE_HAS_USART2) ||                                  \
    defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
#define RK_BOARD_CONSOLE_IRQ_LOWEST_PRIO ((1U << RK_CONF_NPRIO_BITS) - 1U)
#define RK_BOARD_CONSOLE_IRQ_PRIO_SHIFT (8U - RK_CONF_NPRIO_BITS)

static RK_BOOL kBoardConsoleRxIrqEnabled_;

static void kBoardConsoleNvicPrioritySet_(ULONG const irqn,
                                          UINT const priority)
{
    ULONG const regAddress = RK_CORE_NVIC_IPR_BASE + (irqn & ~3UL);
    ULONG const shift = (irqn & 3UL) * 8UL;
    ULONG const mask = 0xFFUL << shift;
    ULONG const value =
        ((ULONG)((priority << RK_BOARD_CONSOLE_IRQ_PRIO_SHIFT) & 0xFFU)) <<
        shift;
    volatile ULONG *const regPtr = (volatile ULONG *)regAddress;

    *regPtr = (*regPtr & ~mask) | value;
}

static void kBoardConsoleNvicEnable_(ULONG const irqn)
{
    volatile ULONG *const regPtr =
        (volatile ULONG *)(RK_CORE_NVIC_BASE + ((irqn / 32UL) * 4UL));

    *regPtr = 1UL << (irqn & 31UL);
}
#endif

#define RK_CONSOLE_SERVICE_RX_EVENT RK_EVENT_1
#define RK_CONSOLE_CMD_WRITE (1UL)

typedef struct RK_STRUCT_CONSOLE_REQUEST
{
    ULONG command;
    CHAR const *bufPtr;
    ULONG bytes;
    ULONG reserved;
} RK_CONSOLE_REQUEST;

typedef struct RK_STRUCT_CONSOLE_REPLY
{
    RK_ERR status;
    ULONG bytes;
} RK_CONSOLE_REPLY;

static RK_TASK_HANDLE kConsoleServiceTaskHandle_;
static RK_STACK kConsoleServiceStack_[RK_CONF_CONSOLE_SERVICE_STACKSIZE]
    K_ALIGN(8);
static RK_BOOL kConsoleServiceMesgInit_;
static RK_CONSOLE_RX_CBK kConsoleRxOwner_;
static BYTE kConsoleRxBuf_[RK_CONF_CONSOLE_RX_BUFFER_BYTES];
static volatile ULONG kConsoleRxRead_;
static volatile ULONG kConsoleRxWrite_;
static volatile ULONG kConsoleRxCount_;
static volatile ULONG kConsoleRxDropped_;

static VOID kConsoleServiceTask_(VOID *args);
static VOID kConsoleRxByteFromIsr_(BYTE ch);
static VOID kConsoleRawPutc_(CHAR c);
static INT kConsoleRawGetc_(CHAR *chPtr);
static VOID kBoardConsoleRxInterruptEnable_(VOID);

#if defined(STM32F401xE)
/*
 * Nucleo-F401RE exposes the ST-LINK virtual COM port through USART2:
 * PA2 = USART2_TX, PA3 = USART2_RX, both using alternate function AF7.
 */
static void kBoardConsolePinsInit_(void)
{
    volatile unsigned long fence;

    /* Enable peripheral clocks before touching GPIOA or USART2 registers. */
    K_F401RE_RCC_AHB1ENR |= K_F401RE_RCC_AHB1ENR_GPIOAEN;
    K_F401RE_RCC_APB1ENR |= K_F401RE_RCC_APB1ENR_USART2EN;

    fence = K_F401RE_RCC_AHB1ENR;
    fence = K_F401RE_RCC_APB1ENR;
    (void)fence;

    /* Put PA2/PA3 in AF7 mode with push-pull output and a pull-up on RX. */
    K_F401RE_GPIOA_MODER &= ~((3UL << (2U * 2U)) | (3UL << (3U * 2U)));
    K_F401RE_GPIOA_MODER |= ((2UL << (2U * 2U)) | (2UL << (3U * 2U)));
    K_F401RE_GPIOA_OTYPER &= ~((1UL << 2U) | (1UL << 3U));
    K_F401RE_GPIOA_OSPEEDR |= ((2UL << (2U * 2U)) | (2UL << (3U * 2U)));
    K_F401RE_GPIOA_PUPDR &= ~((3UL << (2U * 2U)) | (3UL << (3U * 2U)));
    K_F401RE_GPIOA_PUPDR |= (1UL << (3U * 2U));
    K_F401RE_GPIOA_AFRL &= ~((0xFUL << (2U * 4U)) |
                             (0xFUL << (3U * 4U)));
    K_F401RE_GPIOA_AFRL |= ((7UL << (2U * 4U)) | (7UL << (3U * 4U)));
}

/*
 * Decode the APB1 prescaler field from RCC_CFGR. USART2 is clocked from APB1,
 * so the baud-rate divider must use this derived peripheral clock, not the
 * raw core clock.
 */
static unsigned long kBoardConsoleApb1Divisor_(void)
{
    switch ((K_F401RE_RCC_CFGR & K_F401RE_RCC_CFGR_PPRE1_MASK) >>
            K_F401RE_RCC_CFGR_PPRE1_SHIFT)
    {
        case 4UL:
            return (2UL);
        case 5UL:
            return (4UL);
        case 6UL:
            return (8UL);
        case 7UL:
            return (16UL);
        default:
            return (1UL);
    }
}

static unsigned long kBoardConsoleUsart2Brr_(void)
{
    unsigned long coreClock = RK_gSysCoreClock;

    if (coreClock == 0UL)
    {
        coreClock = RK_CONF_EFFECTIVE_SYSCORECLK;
    }
    if (coreClock == 0UL)
    {
        coreClock = 16000000UL;
    }

    unsigned long const apb1Clock = coreClock / kBoardConsoleApb1Divisor_();
    return ((apb1Clock + (RK_BOARD_CONSOLE_BAUD / 2UL)) /
            RK_BOARD_CONSOLE_BAUD);
}

#endif

static ULONG kConsoleRingNext_(ULONG const pos, ULONG const capacity)
{
    ULONG next = pos + 1UL;

    if (next >= capacity)
    {
        next = 0UL;
    }

    return (next);
}

static RK_ERR kConsoleRxRingWrite_(BYTE const ch)
{
    RK_ERR err = RK_ERR_SUCCESS;

    RK_CR_AREA
    RK_CR_ENTER
    if (kConsoleRxCount_ >= (ULONG)sizeof(kConsoleRxBuf_))
    {
        kConsoleRxDropped_++;
        err = RK_ERR_BUFFER_FULL;
    }
    else
    {
        kConsoleRxBuf_[kConsoleRxWrite_] = ch;
        kConsoleRxWrite_ =
            kConsoleRingNext_(kConsoleRxWrite_,
                              (ULONG)sizeof(kConsoleRxBuf_));
        kConsoleRxCount_++;
    }
    RK_CR_EXIT

    return (err);
}

static RK_BOOL kConsoleRxRingRead_(BYTE *const chPtr)
{
    RK_BOOL copied = RK_FALSE;

    RK_CR_AREA
    RK_CR_ENTER
    if ((chPtr != NULL) && (kConsoleRxCount_ > 0UL))
    {
        *chPtr = kConsoleRxBuf_[kConsoleRxRead_];
        kConsoleRxRead_ =
            kConsoleRingNext_(kConsoleRxRead_,
                              (ULONG)sizeof(kConsoleRxBuf_));
        kConsoleRxCount_--;
        copied = RK_TRUE;
    }
    RK_CR_EXIT

    return (copied);
}

static RK_BOOL kConsoleIrqMasked_(VOID)
{
    unsigned primask;
    unsigned basepri;

    RK_ASM volatile("MRS %0, PRIMASK" : "=r"(primask));
    RK_ASM volatile("MRS %0, BASEPRI" : "=r"(basepri));

    return (((primask != 0U) || (basepri != 0U)) ? RK_TRUE : RK_FALSE);
}

static VOID kConsoleServiceSignal_(RK_TASK_EVENT const event)
{
    if ((kConsoleServiceTaskHandle_ != NULL) &&
        (kKernelRunning() == RK_TRUE))
    {
        kEventSet(kConsoleServiceTaskHandle_, event);
    }
}

static RK_BOOL kConsoleServiceCanCallTx_(VOID)
{
    if ((kConsoleServiceTaskHandle_ == NULL) ||
        (kConsoleServiceMesgInit_ != RK_TRUE) ||
        (kKernelRunning() != RK_TRUE))
    {
        return (RK_FALSE);
    }

    if (kIsISR())
    {
        return (RK_FALSE);
    }

    if ((kIsISR() == RK_FALSE) &&
        (kTaskGetRunningHandle() == kConsoleServiceTaskHandle_))
    {
        return (RK_FALSE);
    }

    if ((kIsISR() == RK_FALSE) && (kConsoleIrqMasked_() == RK_TRUE))
    {
        return (RK_FALSE);
    }

    return (RK_TRUE);
}

static RK_ERR kConsoleCallerReadValid_(RK_SYNCH_CALL_DATA const *const callPtr,
                                       CHAR const *const bufPtr,
                                       ULONG const bytes)
{
    if (bytes == 0UL)
    {
        return (RK_ERR_SUCCESS);
    }
    if ((callPtr == NULL) || (bufPtr == NULL))
    {
        return (RK_ERR_OBJ_NULL);
    }

    RK_TCB *callerPtr = NULL;
    RK_ERR const err = kTaskHandleResolve(callPtr->caller, &callerPtr);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    if ((callerPtr->savedControl & 0x1UL) == 0UL)
    {
        return (RK_ERR_SUCCESS);
    }

    return ((kMpuUserReadValid(callerPtr, bufPtr, bytes) == RK_TRUE)
                ? RK_ERR_SUCCESS
                : RK_ERR_INVALID_PARAM);
}

static VOID kConsoleRawWrite_(CHAR const *const bufPtr,
                              ULONG const bytes)
{
    for (ULONG i = 0UL; i < bytes; i++)
    {
        kConsoleRawPutc_(bufPtr[i]);
    }
}

static RK_ERR kConsoleTxRequestExecute_(
    RK_SYNCH_CALL_DATA const *const callPtr,
    RK_CONSOLE_REQUEST const *const reqPtr)
{
    RK_ERR err;

    if (reqPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    if (reqPtr->command != RK_CONSOLE_CMD_WRITE)
    {
        return (RK_ERR_INVALID_PARAM);
    }
    if (reqPtr->bytes > RK_CONSOLE_WRITE_MAX_BYTES)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    err = kConsoleCallerReadValid_(callPtr, reqPtr->bufPtr, reqPtr->bytes);
    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    kConsoleRawWrite_(reqPtr->bufPtr, reqPtr->bytes);
    return (RK_ERR_SUCCESS);
}

static VOID kConsoleRxDispatch_(BYTE const ch)
{
    RK_CONSOLE_RX_CBK cbk;

    RK_CR_AREA
    RK_CR_ENTER
    cbk = kConsoleRxOwner_;
    RK_CR_EXIT

    if (cbk != NULL)
    {
        cbk(ch);
    }
}

static RK_BOOL kConsoleServiceDrainRx_(VOID)
{
    BYTE ch;
    RK_BOOL didWork = RK_FALSE;

    while (kConsoleRxRingRead_(&ch) == RK_TRUE)
    {
        kConsoleRxDispatch_(ch);
        didWork = RK_TRUE;
    }

    return (didWork);
}

static RK_BOOL kConsoleServiceAcceptTx_(RK_TICK const timeout)
{
    RK_SYNCH_CALL_DATA call;
    RK_CONSOLE_REQUEST req;
    RK_CONSOLE_REPLY reply;
    ULONG reqBytes = 0UL;

    RK_ERR const err = kSynchMesgAccept(&call, &req, &reqBytes, timeout);
    if (err != RK_ERR_SUCCESS)
    {
        return (RK_FALSE);
    }

    reply.bytes = 0UL;
    if (reqBytes != (ULONG)sizeof(req))
    {
        reply.status = RK_ERR_INVALID_MSG_SIZE;
    }
    else
    {
        reply.status = kConsoleTxRequestExecute_(&call, &req);
        if (reply.status == RK_ERR_SUCCESS)
        {
            reply.bytes = req.bytes;
        }
    }

    kSynchMesgReply(&call, &reply, (ULONG)sizeof(reply));
    return (RK_TRUE);
}

static VOID kConsoleServiceTask_(VOID *args)
{
    K_UNUSE(args);

    while (1)
    {
        kConsoleServiceDrainRx_();
        kEventGet(RK_CONSOLE_SERVICE_RX_EVENT, RK_OPT_EVENT_ANY,
                        NULL, RK_NO_WAIT);
        kConsoleServiceAcceptTx_(
            RK_CONF_CONSOLE_SERVICE_POLL_TICKS);
    }
}

static VOID kConsoleRxByteFromIsr_(BYTE const ch)
{
    if (kConsoleRxRingWrite_(ch) == RK_ERR_SUCCESS)
    {
        kConsoleServiceSignal_(RK_CONSOLE_SERVICE_RX_EVENT);
    }
}

void kBoardConsoleInit(void)
{
    static unsigned char initDone;

    if (initDone != 0U)
    {
        return;
    }

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
    kBoardConsolePinsInit_();
    /* Configure USART2 as 8N1, 115200 baud, transmitter and receiver enabled. */
    K_F401RE_USART2_CR1 = 0UL;
    K_F401RE_USART2_BRR = kBoardConsoleUsart2Brr_();
    K_F401RE_USART2_CR1 =
        K_F401RE_USART2_CR1_UE | K_F401RE_USART2_CR1_TE |
        K_F401RE_USART2_CR1_RE;
#elif defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
    unsigned long coreClock = RK_gSysCoreClock;

    if (coreClock == 0UL)
    {
        coreClock = RK_CONF_EFFECTIVE_SYSCORECLK;
    }
    if (coreClock == 0UL)
    {
        coreClock = 25000000UL;
    }

    CMSDK_UART_CTRL = 0UL;
    CMSDK_UART_BAUDDIV =
        ((coreClock + (CMSDK_UART_BAUD / 2UL)) / CMSDK_UART_BAUD);
    CMSDK_UART_CTRL = CMSDK_UART_CTRL_TXEN | CMSDK_UART_CTRL_RXEN;
#endif

    initDone = 1U;
}

static VOID kBoardConsoleRxInterruptEnable_(VOID)
{
    kBoardConsoleInit();

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
    if (kBoardConsoleRxIrqEnabled_ == RK_TRUE)
    {
        return;
    }

    kBoardConsoleNvicPrioritySet_(K_F401RE_USART2_IRQN,
                                  RK_BOARD_CONSOLE_IRQ_LOWEST_PRIO);
    K_F401RE_USART2_CR1 |= K_F401RE_USART2_CR1_RXNEIE;
    kBoardConsoleNvicEnable_(K_F401RE_USART2_IRQN);

    kBoardConsoleRxIrqEnabled_ = RK_TRUE;
#elif defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
    if (kBoardConsoleRxIrqEnabled_ == RK_TRUE)
    {
        return;
    }

    kBoardConsoleNvicPrioritySet_(CMSDK_UART0_IRQN,
                                  RK_BOARD_CONSOLE_IRQ_LOWEST_PRIO);
    CMSDK_UART_INTSTATUS = CMSDK_UART_INTSTATUS_RXIRQ;
    CMSDK_UART_CTRL |= CMSDK_UART_CTRL_RXIRQEN;
    kBoardConsoleNvicEnable_(CMSDK_UART0_IRQN);

    kBoardConsoleRxIrqEnabled_ = RK_TRUE;
#endif
}

RK_ERR kConsoleServiceInit(VOID)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    if (kIsISR())
    {
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    kBoardConsoleInit();

    if (kConsoleServiceTaskHandle_ == NULL)
    {
        RK_ERR const err = kTaskInitPrivileged(
            &kConsoleServiceTaskHandle_, kConsoleServiceTask_, RK_NO_ARGS,
            "Console", kConsoleServiceStack_,
            RK_CONF_CONSOLE_SERVICE_STACKSIZE,
            RK_CONF_CONSOLE_SERVICE_PRIO, RK_PREEMPT);
        if (err != RK_ERR_SUCCESS)
        {
            return (err);
        }
    }

    if (kConsoleServiceMesgInit_ != RK_TRUE)
    {
        RK_ERR const err =
            kSynchMesgInit(kConsoleServiceTaskHandle_,
                           (ULONG)sizeof(RK_CONSOLE_REQUEST));
        if ((err != RK_ERR_SUCCESS) && (err != RK_ERR_HAS_OWNER))
        {
            return (err);
        }
        kConsoleServiceMesgInit_ = RK_TRUE;
    }

    kBoardConsoleRxInterruptEnable_();
    return (RK_ERR_SUCCESS);
}

RK_ERR kConsoleRxClaim(RK_CONSOLE_RX_CBK const cbk)
{
    RK_ERR err = RK_ERR_SUCCESS;
    RK_CONSOLE_RX_CBK oldOwner;

    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    if (kIsISR())
    {
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (cbk == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    oldOwner = NULL;

    RK_CR_AREA
    RK_CR_ENTER
    if ((kConsoleRxOwner_ == NULL) || (kConsoleRxOwner_ == cbk))
    {
        oldOwner = kConsoleRxOwner_;
        kConsoleRxOwner_ = cbk;
    }
    else
    {
        err = RK_ERR_CHANNEL_BUSY;
    }
    RK_CR_EXIT

    if (err != RK_ERR_SUCCESS)
    {
        return (err);
    }

    err = kConsoleServiceInit();
    if (err != RK_ERR_SUCCESS)
    {
        RK_CR_ENTER
        if ((oldOwner == NULL) && (kConsoleRxOwner_ == cbk))
        {
            kConsoleRxOwner_ = NULL;
        }
        RK_CR_EXIT
    }

    return (err);
}

RK_ERR kConsoleRxRelease(RK_CONSOLE_RX_CBK const cbk)
{
    RK_ERR err = RK_ERR_SUCCESS;

    if (kSyscallRequired() == RK_TRUE)
    {
        return (RK_ERR_INVALID_PHASE);
    }

    if (kIsISR())
    {
        return (RK_ERR_INVALID_ISR_PRIMITIVE);
    }

    if (cbk == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }

    RK_CR_AREA
    RK_CR_ENTER
    if (kConsoleRxOwner_ == cbk)
    {
        kConsoleRxOwner_ = NULL;
    }
    else if (kConsoleRxOwner_ == NULL)
    {
        err = RK_ERR_CHANNEL_NOT_ACTIVE;
    }
    else
    {
        err = RK_ERR_NOT_OWNER;
    }
    RK_CR_EXIT

    return (err);
}

void kBoardConsoleRxIsrEnable(RK_CONSOLE_RX_ISR_CBK const cbk)
{
    kConsoleRxClaim(cbk);
}

static VOID kConsoleRawPutc_(CHAR const c)
{
    kBoardConsoleInit();

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
    /* Poll TXE; the console is intentionally simple and synchronous. */
    while ((K_F401RE_USART2_SR & K_F401RE_USART2_SR_TXE) == 0UL)
    {
    }
    K_F401RE_USART2_DR = (unsigned long)((unsigned char)c);
#elif defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
    while ((CMSDK_UART_STATE & CMSDK_UART_STATE_TXBF) != 0UL)
    {
    }
    CMSDK_UART_DATA = (unsigned long)((unsigned char)c);
#else
    (void)c;
#endif
}

static VOID kConsoleWriteAttrInit_(RK_CONSOLE_REQUEST *const reqPtr,
                                   RK_CONSOLE_REPLY *const replyPtr,
                                   RK_SYNCH_ATTR *const attrPtr,
                                   ULONG *const replyBytesPtr,
                                   CHAR const *const bufPtr,
                                   ULONG const bytes)
{
    reqPtr->command = RK_CONSOLE_CMD_WRITE;
    reqPtr->bufPtr = bufPtr;
    reqPtr->bytes = bytes;
    reqPtr->reserved = 0UL;

    replyPtr->status = RK_ERR_ERROR;
    replyPtr->bytes = 0UL;
    *replyBytesPtr = 0UL;

    attrPtr->reqPtr = reqPtr;
    attrPtr->reqBytes = (ULONG)sizeof(*reqPtr);
    attrPtr->replyPtr = replyPtr;
    attrPtr->replyMaxBytes = (ULONG)sizeof(*replyPtr);
    attrPtr->replyBytesPtr = replyBytesPtr;
}

static RK_ERR kConsoleWriteReplyStatus_(RK_ERR const callErr,
                                        RK_CONSOLE_REPLY const *const replyPtr,
                                        ULONG const replyBytes)
{
    if (callErr != RK_ERR_SUCCESS)
    {
        return (callErr);
    }
    if ((replyPtr == NULL) || (replyBytes != (ULONG)sizeof(*replyPtr)))
    {
        return (RK_ERR_INVALID_MSG_SIZE);
    }

    return (replyPtr->status);
}

static RK_ERR kConsoleWriteViaService_(CHAR const *const bufPtr,
                                       ULONG const bytes)
{
    RK_CONSOLE_REQUEST req;
    RK_CONSOLE_REPLY reply;
    RK_SYNCH_ATTR attr;
    ULONG replyBytes;

    kConsoleWriteAttrInit_(&req, &reply, &attr, &replyBytes, bufPtr, bytes);

    RK_ERR const err =
        kSynchMesgCall(kConsoleServiceTaskHandle_, &attr, RK_WAIT_FOREVER);
    return (kConsoleWriteReplyStatus_(err, &reply, replyBytes));
}

void kPutc(char const c)
{
    if (kSyscallRequired() == RK_TRUE)
    {
        kConsoleWrite(&c, 1UL);
        return;
    }

    kConsoleRawPutc_(c);
}

RK_ERR kConsoleWrite(CHAR const *bufPtr, ULONG bytes)
{
    RK_CONSOLE_REQUEST req;
    RK_CONSOLE_REPLY reply;
    RK_SYNCH_ATTR attr;
    ULONG replyBytes;

    if (bytes == 0UL)
    {
        return (RK_ERR_SUCCESS);
    }
    if (bufPtr == NULL)
    {
        return (RK_ERR_OBJ_NULL);
    }
    if (bytes > RK_CONSOLE_WRITE_MAX_BYTES)
    {
        return (RK_ERR_INVALID_PARAM);
    }

    if (kSyscallRequired() == RK_TRUE)
    {
        kConsoleWriteAttrInit_(&req, &reply, &attr, &replyBytes, bufPtr,
                               bytes);
        RK_ERR const err = (RK_ERR)kSyscallInvoke4(
            RK_SYSCALL_CONSOLE_WRITE, (ULONG)(UINTPTR)&attr,
            (ULONG)RK_WAIT_FOREVER, 0UL, 0UL);
        return (kConsoleWriteReplyStatus_(err, &reply, replyBytes));
    }

    if (kConsoleServiceCanCallTx_() == RK_TRUE)
    {
        return (kConsoleWriteViaService_(bufPtr, bytes));
    }

    kConsoleRawWrite_(bufPtr, bytes);
    return (RK_ERR_SUCCESS);
}

RK_ERR kConsoleWriteSyscall(RK_EXCEPTION_FRAME *const framePtr,
                            RK_SYNCH_ATTR const *const attrPtr,
                            RK_TICK const timeout)
{
    if ((kConsoleServiceTaskHandle_ == NULL) ||
        (kConsoleServiceMesgInit_ != RK_TRUE))
    {
        kSyscallTaskClear(RK_gRunPtr);
        return (RK_ERR_OBJ_NOT_INIT);
    }

    return (kSynchMesgCallSyscall(framePtr, kConsoleServiceTaskHandle_,
                                  attrPtr, timeout));
}

void kPuts(char const *str)
{
    if (str == (char const *)0)
    {
        return;
    }

    while (*str != '\0')
    {
        ULONG bytes = 0UL;

        while ((str[bytes] != '\0') &&
               (bytes < RK_CONSOLE_WRITE_MAX_BYTES))
        {
            bytes++;
        }

        if (kConsoleWrite(str, bytes) != RK_ERR_SUCCESS)
        {
            return;
        }

        str += bytes;
    }
}

static INT kConsoleRawGetc_(CHAR *const chPtr)
{
    if (chPtr == (char *)0)
    {
        return (0);
    }

    kBoardConsoleInit();

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
    if ((K_F401RE_USART2_SR & K_F401RE_USART2_SR_RXNE) == 0UL)
    {
        return (0);
    }

    *chPtr = (char)(K_F401RE_USART2_DR & 0xFFUL);
    return (1);
#elif defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
    if ((CMSDK_UART_STATE & CMSDK_UART_STATE_RXBF) == 0UL)
    {
        return (0);
    }

    *chPtr = (char)(CMSDK_UART_DATA & 0xFFUL);
    return (1);
#else
    return (0);
#endif
}

int kConsoleGetc(char *chPtr)
{
    return (kConsoleRawGetc_(chPtr));
}

#if defined(RK_BOARD_CONSOLE_HAS_USART2)
void USART2_IRQHandler(void)
{
    while ((K_F401RE_USART2_SR & K_F401RE_USART2_SR_RXNE) != 0UL)
    {
        BYTE const ch = (BYTE)(K_F401RE_USART2_DR & 0xFFUL);

        kConsoleRxByteFromIsr_(ch);
    }
}
#endif

#if defined(RK_BOARD_CONSOLE_HAS_CMSDK_UART)
void UART0_Handler(void)
{
    do
    {
        CMSDK_UART_INTSTATUS = CMSDK_UART_INTSTATUS_RXIRQ;
        while ((CMSDK_UART_STATE & CMSDK_UART_STATE_RXBF) != 0UL)
        {
            BYTE const ch = (BYTE)(CMSDK_UART_DATA & 0xFFUL);

            kConsoleRxByteFromIsr_(ch);
        }
    } while (((CMSDK_UART_STATE & CMSDK_UART_STATE_RXBF) != 0UL) ||
             ((CMSDK_UART_INTSTATUS & CMSDK_UART_INTSTATUS_RXIRQ) != 0UL));
}
#endif

int _write(int file, char const *ptr, int len)
{
    (void)file;

    /*
     * newlib calls _write() for printf-family output. Route all file
     * descriptors to the kernel console service because RK01 has no
     * filesystem; the service falls back to raw UART when the scheduler is not
     * available.
     */
    int written = 0;
    while (written < len)
    {
        ULONG chunk = (ULONG)(len - written);

        if (chunk > RK_CONSOLE_WRITE_MAX_BYTES)
        {
            chunk = RK_CONSOLE_WRITE_MAX_BYTES;
        }

        if (kConsoleWrite(&ptr[written], chunk) != RK_ERR_SUCCESS)
        {
            break;
        }

        written += (int)chunk;
    }

    return (written);
}
