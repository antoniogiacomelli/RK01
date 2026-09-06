/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                  */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

ENTRY(Reset_Handler)
_Min_Heap_Size  = 0x00000200; /* u can use the heap to place partition pools */
_Min_Stack_Size = 0x00000800; /* required amount of stack */
/* Define memory layout */
MEMORY
{
  FLASH      (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
  FS_FLASH   (rx)  : ORIGIN = 0x08040000, LENGTH = 256K
  TASK_RAM   (rw)  : ORIGIN = 0x20000000, LENGTH = 64K
  KERNEL_RAM (rwx) : ORIGIN = 0x20010000, LENGTH = 32K
}

__rk_flash_begin = ORIGIN(FLASH);
__rk_flash_end   = ORIGIN(FLASH) + LENGTH(FLASH);
__rk_fs_flash_begin = ORIGIN(FS_FLASH);
__rk_fs_flash_end   = ORIGIN(FS_FLASH) + LENGTH(FS_FLASH);

__rk_task_ram_begin = ORIGIN(TASK_RAM);
__rk_task_ram_end   = ORIGIN(TASK_RAM) + LENGTH(TASK_RAM);

__rk_kernel_ram_begin = ORIGIN(KERNEL_RAM);
__rk_kernel_ram_end   = ORIGIN(KERNEL_RAM) + LENGTH(KERNEL_RAM);

/* Stack pointer (MSP) is set to end of privileged kernel RAM */
__StackTop   = ORIGIN(KERNEL_RAM) + LENGTH(KERNEL_RAM);
_estack      = __StackTop;          /* CMSIS default name    */
PROVIDE(__stack = _estack);         /* compatibility symbol  */

/* Sections */
SECTIONS
{
  .text :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))     /* Vector table */
    *(.text*)                /* Application code */
    *(.rodata*)              /* Read-only data */
    KEEP(*(.init))
    KEEP(*(.fini))
    KEEP(*(.ARM.exidx*))
    . = ALIGN(4);
    __etext = .;
  } > FLASH

  .rk_app_domain_data : AT(__etext)
  {
    . = ALIGN(0x4000);
    __rk_task_ram_static_begin = .;
    __rk_app_domain_begin = .;
    __rk_app_domain_data_begin = .;
    *app/src/*.o(.data* .sdata*)
    */app/src/*.o(.data* .sdata*)
    *app/examples/*.o(.data* .sdata*)
    */app/examples/*.o(.data* .sdata*)
    . = ALIGN(4);
    __rk_app_domain_data_end = .;
  } > TASK_RAM
  __rk_app_domain_data_load = LOADADDR(.rk_app_domain_data);
  __rk_app_domain_data_load_end = __rk_app_domain_data_load + SIZEOF(.rk_app_domain_data);

  .rk_app_domain_bss (NOLOAD) :
  {
    . = ALIGN(8);
    __rk_app_domain_bss_begin = .;
    *(.rk_app_ram*)
    *(.rk_task_stack*)
    *app/src/*.o(.bss* .sbss* COMMON)
    */app/src/*.o(.bss* .sbss* COMMON)
    *app/examples/*.o(.bss* .sbss* COMMON)
    */app/examples/*.o(.bss* .sbss* COMMON)
    . = ALIGN(8);
    __rk_app_domain_bss_end = .;
    ASSERT((__rk_app_domain_bss_end - __rk_app_domain_begin) <= 0x4000, "RK implicit application domain exceeds 16K");
    . = __rk_app_domain_begin + 0x4000;
    __rk_app_domain_end = .;
    __rk_task_stack_begin = __rk_app_domain_begin;
    __rk_task_stack_end = __rk_app_domain_bss_end;
  } > TASK_RAM

  .rk_domain_ram (NOLOAD) :
  {
    . = ALIGN(8);
    __rk_domain_ram_begin = .;

    KEEP(*(.rk_domain_ram*))
    *(.rk_task_ram*)
    . = ALIGN(8);
    __rk_domain_ram_end = .;
  } > TASK_RAM

  .rk_task_noinit (NOLOAD) :
  {
    . = ALIGN(8);
    __rk_task_noinit_begin = .;
    *(.rk_task_noinit*)
    . = ALIGN(8);
    __rk_task_noinit_end = .;
    __rk_task_ram_static_end = .;
  } > TASK_RAM

  .rk_shared_ram (NOLOAD) :
  {
    . = ALIGN(1024);
    __rk_shared_ram_begin = .;
    __rk_shared_bss_begin = .;
    *(.rk_shared_bss*)
    *(.rk_shared_ram*)
    . = ALIGN(4);
    __rk_shared_bss_end = .;
    __rk_shared_noinit_begin = .;
    *(.rk_shared_noinit*)
    . = ALIGN(4);
    __rk_shared_noinit_end = .;
    ASSERT(__rk_shared_noinit_end <= (__rk_shared_ram_begin + 1024), "RK shared RAM exceeds 1024 bytes");
    . = __rk_shared_ram_begin + 1024;
    __rk_shared_ram_end = .;
  } > TASK_RAM

  /* Initialised data: copied from flash to RAM */
 .data : AT(__rk_app_domain_data_load_end)
  {
   . = ALIGN(4);
    _sdata = .;
    *(.rk_kernel_data*)
    *(.data*)
    _edata = .;
    . = ALIGN(4);
  } > KERNEL_RAM

  /* Uninitialised data */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.rk_kernel_bss*)
    *(.bss*)
    *(COMMON)
    _ebss = .;
  . = ALIGN(4);
  } > KERNEL_RAM

  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit)
    . = ALIGN(4);
  } > KERNEL_RAM

  /* Provide symbol to copy .data from flash */
  _sidata = LOADADDR(.data);


    /* User_heap_stack section, used to check that there is enough "RAM" Ram  type memory left */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } > KERNEL_RAM

  /* this is actually doing nothing, as we link libcnano so, just in case*/
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

}
