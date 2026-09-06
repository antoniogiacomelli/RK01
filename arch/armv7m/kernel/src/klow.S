/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                  */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

.syntax unified /* thumb2 */
.text
.align 4

/* task status values */
.equ READY,   0x10
.equ RUNNING, 0x20
.equ FAULT_PENDING, 0x4A

/* armv7m reg addresses */
.equ SCB_ICSR,   0xE000ED04 /* base icsr */
.equ STICK_CTRL, 0xE000E010 /* systick csr */
/* arvm7m reg values */
.equ ISCR_SETPSV, (1 << 28)
.equ ISCR_CLRPSV, (1 << 27)
.equ STICK_CLRP, (1 << 30)
.equ STICK_ON,  0x0F
.equ STICK_OFF, 0x00

/* kernel specifics  */
.equ APP_START_UP_IMM,   0xAA
.equ FAULT_SVC,          0xFFFFFC72
.equ FAULT_PENDSV_EXC_RETURN, 0xFFFFFC71
.equ STACK_GUARD, 0x0BADC0DE
.equ FAULT_STACKOVERVLOW, 0xFAFAFAFA
.equ KERNEL_PHASE_BOOT, 0
.equ KERNEL_PHASE_RUNNING, 1
.equ SP_OFFSET, 0
.equ STATUS_OFFSET, 4
.equ RUNCNTR_OFFSET, 8
.equ LR_OFFSET, 12
.equ STACKADDR, 16
.equ FP_CONTEXT_OFFSET, 0x48



.global kApplicationInit
.type kApplicationInit, %function
.thumb_func

.global kErrHandler  /* defined in kerr.c */
.type kErrHandler, %function
.thumb_func

.global kMpuLoadRunTask
.type kMpuLoadRunTask, %function
.thumb_func

.global kMpuSaveRunTaskControl
.type kMpuSaveRunTaskControl, %function
.thumb_func

.global kMpuGetRunTaskControl
.type kMpuGetRunTaskControl, %function
.thumb_func

.global kSyscallDispatch
.type kSyscallDispatch, %function
.thumb_func

.global kMemManageFault
.type kMemManageFault, %function
.thumb_func

.global SVC_Handler
.type SVC_Handler, %function
.thumb_func
.text
.align 4
SVC_Handler:       /* we start-up from msp               */
    TST LR, #4     /* for MSP, LR rightmost nibble is x9 (0b1001) */
    ITE EQ
    MRSEQ R0, MSP
    MRSNE R0, PSP
#if (__FPU_PRESENT == 1)
    /* EXC_RETURN bit 4 clear means the hardware stacked an extended FP frame
       below the integer frame. C handlers always receive the integer frame. */
    TST LR, #0x10
    IT EQ
    ADDEQ R0, R0, #FP_CONTEXT_OFFSET
#endif
    LDR R1, [R0, #24] /* <-  the instruction that brought us here       */
    LDRB R2, [R1,#-2] /* <- walk two bytes back on code memory,
                             get the immediate      */
    CMP R2, #APP_START_UP_IMM
    BNE SVC_DISPATCH
    TST LR, #4
    BNE SVC_DISPATCH
    B SVC_STARTUP
SVC_DISPATCH:
    PUSH {R4, LR}
    MOV R4, LR
    MOV R1, LR
    BL  kSyscallDispatch
    MOV LR, R4
    POP {R4}
    ADD SP, SP, #4
    BX  LR
SVC_STARTUP:
    LDR R3, =RK_gStartupSvcArmed
    LDR R2, [R3]
    CMP R2, #1
    BNE ERR
    LDR R3, =RK_gKernelPhase
    LDR R2, [R3]
    CMP R2, #KERNEL_PHASE_BOOT
    BNE ERR
    MOVS R2, #0
    LDR R3, =RK_gStartupSvcArmed
    STR R2, [R3]
    MOVS R2, #KERNEL_PHASE_RUNNING
    LDR R3, =RK_gKernelPhase
    STR R2, [R3]
    B USRSTARTUP
    ERR:
    LDR R0, =FAULT_SVC
    BL  kErrHandler
    B ERR

/* start user application */
.global USRSTARTUP
.type USRSTARTUP, %function
.text
.align 4
.thumb_func
 USRSTARTUP:                  /* get camera-ready:                        */
    CPSID I
    BL kMpuLoadRunTask        /* install selected task regions first       */
    BL kMpuGetRunTaskControl
    LDR R1, =RK_gRunPtr
    LDR R2, [R1]              /* R2 = RK_gRunPtr                           */
    MOVS R12, #RUNNING
    STR R12, [R2, #STATUS_OFFSET]
    #ifndef NDEBUG
    MOVS R12, #0x01
    STR R12, [R2, #RUNCNTR_OFFSET]
    #endif
    LDR LR, [R2, #LR_OFFSET] /* LR = RK_gRunPtr->savedLR                   */
    LDR R2, [R2, #SP_OFFSET] /* R2 = RK_gRunPtr->sp                        */
    LDMIA R2!, {R4-R11}      /* 'POP' R4-R11 as    assembled on kInitTcb_  */
    MSR PSP, R2              /* update PSP after 'popping '                */
    MSR CONTROL, R0           /* apply thread privilege after PSP is valid  */
    DSB                      /* yelling to data bus */
    ISB                      /* exorcising the ins pipe */
    CPSIE I                  /* red lights on */
    ISB
    BX LR                    /* play                    */
.global SysTick_Handler
.type SysTick_Handler, %function
.thumb_func
.text
.align 4
SysTick_Handler:


    PUSH {R0, LR}               /* Save LR, r0 for align */
    BL kTickHandler
    CMP R0, #1
    BNE NOSWTCH
    /* fallthrough */
    CPSID I
    LDR R0, =SCB_ICSR
    LDR R1, =ISCR_SETPSV
    STR R1, [R0]
    DSB
    CPSIE I
    ISB
    NOSWTCH:
    POP {R0, LR}
    BX LR


#ifdef __KDEF_STACKOVFLW
.macro  CHECK_STACK_GUARD tcbReg, successLabel
        LDR   R2, [\tcbReg, #STACKADDR]   /* R2 = &stackAddr */
        LDR   R3, [R2]                     /* R3 = *(stackBottom) */
        LDR   R2, =STACK_GUARD            /* R2 = expected canary */
        CMP   R2, R3
        BEQ   \successLabel
        /*error: fallthrough */
        LDR   R0, =FAULT_STACKOVERVLOW
        LDR   R4, =kErrHandler
        BX    R4
.endm
#endif
/* ctxt swtch */
.global PendSV_Handler
.type PendSV_Handler, %function
.thumb_func
.text
.align 4
PendSV_Handler:
    CPSID I
#ifndef NDEBUG
    TST LR, #4
    BNE PENDSV_EXC_RETURN_OK
    LDR R0, =FAULT_PENDSV_EXC_RETURN
    BL  kErrHandler
PENDSV_BAD_EXC_RETURN:
    B PENDSV_BAD_EXC_RETURN
PENDSV_EXC_RETURN_OK:
#endif

    LDR R0, =RK_gRunPtr
    LDR R1, [R0]
    LDR R2, [R1, #STATUS_OFFSET]
    LDR R3, =FAULT_PENDING
    CMP R2, R3
    BEQ SKIP_OUTGOING_SAVE

    MRS R12, PSP
#if (__FPU_PRESENT == 1)
    /* S0-S15/FPSCR are handled by exception entry. S16-S31 are callee-saved
       task state, so PendSV owns them when EXC_RETURN says FP state is live. */
    TST LR, #0x10
    BNE SKIPSAVEVFP
    VSTMDB R12!, {S16-S31}
    SKIPSAVEVFP:
#endif

    STMDB R12!, {R4-R11}
    LDR R0, =RK_gRunPtr
    LDR R1, [R0]
#ifdef __KDEF_STACKOVFLW
    /* check for stack overflow */
    CHECK_STACK_GUARD R1, SAVE
#endif
    SAVE:
    STR R12, [R1, #SP_OFFSET]     /* save PSP */
    STR LR, [R1, #LR_OFFSET]     /* save LR  */
    MRS R0, CONTROL
    BL kMpuSaveRunTaskControl
    B CALL_SCHEDULER

SKIP_OUTGOING_SAVE:
    MRS R0, CONTROL
    BL kMpuSaveRunTaskControl

CALL_SCHEDULER:
    /* call scheduler */
    BL kSwtch

    BL kMpuLoadRunTask
    BL kMpuGetRunTaskControl

    /* load new task context */
    LDR R1, =RK_gRunPtr
    LDR R1, [R1]
#ifdef __KDEF_STACKOVFLW
    CHECK_STACK_GUARD R1, RESTORE
#endif


    RESTORE:
    /* update run counter and status */
#ifndef NDEBUG
    LDR R3, [R1, #RUNCNTR_OFFSET]
    ADD R3, #1
    STR R3, [R1, #RUNCNTR_OFFSET]
#endif
    MOV R12, #RUNNING
    STR R12, [R1, #STATUS_OFFSET]

    /* get saved LR at TCB */
    LDR LR, [R1, #LR_OFFSET]

    /*  get saved PSP value */
    LDR R2, [R1, #SP_OFFSET]
    LDMIA R2!, {R4-R11}

#if (__FPU_PRESENT == 1)
    /* Restore the software-saved high-FP block only for tasks that saved one. */
    TST LR, #0x10
    BNE SKIPRESTOREVFP
    VLDMIA R2!, {S16-S31}
    SKIPRESTOREVFP:
#endif
 /* assign PSP */

    MSR PSP, R2
    MSR CONTROL, R0
    DSB
    ISB
    CPSIE I
    ISB
    BX LR

.global MemManage_Handler
.type MemManage_Handler, %function
.thumb_func
.text
.align 4
MemManage_Handler:
    TST LR, #4
    ITE EQ
    MRSEQ R0, MSP
    MRSNE R0, PSP
#if (__FPU_PRESENT == 1)
    /* Report the integer frame even when a fault occurred with FP context. */
    TST LR, #0x10
    IT EQ
    ADDEQ R0, R0, #FP_CONTEXT_OFFSET
#endif
    MOV R1, LR
    B kMemManageFault
