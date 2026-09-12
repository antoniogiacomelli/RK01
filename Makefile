# SPDX-License-Identifier: Apache-2.0
#
# RK01 firmware build.
#
# Default target:
#   make
#
# Explicit F401RE build:
#   make -j4 ARCH=armv7m PLATFORM=stm32f401re
#
# Cortex-M33 QEMU build:
#   make -j4 ARCH=armv8m PLATFORM=mps2-an505
#
# Cortex-M4 QEMU build:
#   make -j4 ARCH=armv7m PLATFORM=mps2-an386
#
ARCH ?= armv7m
PLATFORM ?= stm32f401re
TARGET ?= rk01_demo
EXTRA_DEFINES ?=
EXTRA_DEFS ?=
APP_DEFS :=
APP_EXAMPLE_ORIGIN := $(origin APP_EXAMPLE)
APP_EXAMPLE ?= tiny
RK_BUILD_COOKIE ?= $(shell date +%s)
BUILD ?= debug
FPU ?= OFF

CORE_DIR := core
APP_DIR := app
MIDDLEWARE_DIR := middleware
ARCH_DIR := arch/$(ARCH)

BUILD_ROOT ?= build
BUILD_PROFILE ?= $(BUILD)
ifeq ($(origin APP_SRCS), undefined)
ifeq ($(APP_EXAMPLE),tiny)
APP_SRCS := \
    $(APP_DIR)/src/application.c \
    $(APP_DIR)/src/record_domain.c \
    $(APP_DIR)/src/record_server.c \
    $(APP_DIR)/src/tiny_console.c \
    $(APP_DIR)/src/tiny_support.c
else ifeq ($(APP_EXAMPLE),01-fleet)
APP_SRCS := $(APP_DIR)/examples/01_fleet_domains.c
else ifeq ($(APP_EXAMPLE),02-isolated)
APP_SRCS := $(APP_DIR)/examples/02_isolated_tasks.c
else ifeq ($(APP_EXAMPLE),03-watchdog)
APP_SRCS := $(APP_DIR)/examples/03_watchdog.c
else ifeq ($(APP_EXAMPLE),04-sysmon)
APP_SRCS := $(APP_DIR)/examples/04_sysmon.c
APP_DEFS += -DRK_CONF_SYSMON=ON
else ifeq ($(APP_EXAMPLE),05-profile-preempt)
APP_SRCS := $(APP_DIR)/examples/05_profile_preempt.c
else ifeq ($(APP_EXAMPLE),06-profile-ctxsw)
APP_SRCS := $(APP_DIR)/examples/06_profile_ctxsw.c
APP_DEFS += -DRK_CONF_PROFILE_PENDSV=1
else ifeq ($(APP_EXAMPLE),07-signals)
APP_SRCS := $(APP_DIR)/examples/07_signals.c
else ifeq ($(APP_EXAMPLE),reg-origin-scoped)
APP_SRCS := $(APP_DIR)/regression/origin_scoped_constructors.c
else ifeq ($(APP_EXAMPLE),reg-signal-synch)
APP_SRCS := $(APP_DIR)/regression/signal_synch_wait_cleanup.c
APP_DEFS += -DRK_CONF_ERR_CHECK=OFF
else ifeq ($(APP_EXAMPLE),99-showcase)
APP_SRCS := $(APP_DIR)/examples/99_showcase.c
else
$(error Unsupported APP_EXAMPLE '$(APP_EXAMPLE)': use tiny, 01-fleet, 02-isolated, 03-watchdog, 04-sysmon, 05-profile-preempt, 06-profile-ctxsw, 07-signals, reg-origin-scoped, reg-signal-synch, or 99-showcase)
endif
endif

ifeq ($(origin DOMAIN_IMPL_SRCS), undefined)
DOMAIN_IMPL_SRCS :=
ifeq ($(APP_EXAMPLE),tiny)
DOMAIN_IMPL_SRCS += \
    $(APP_DIR)/src/record_domain.c \
    $(APP_DIR)/src/record_server.c
endif
endif

BUILD_APP ?= $(TARGET)-$(APP_EXAMPLE)
BUILD_DIR ?= $(BUILD_ROOT)/$(ARCH)/$(PLATFORM)/$(BUILD_PROFILE)/$(BUILD_APP)
CONFIG_STAMP := $(BUILD_DIR)/.config.stamp

TOOLCHAIN ?= arm-none-eabi
CC := $(TOOLCHAIN)-gcc
OBJCOPY := $(TOOLCHAIN)-objcopy
OBJDUMP := $(TOOLCHAIN)-objdump
SIZE := $(TOOLCHAIN)-size

LINKER_SCRIPT ?= $(ARCH_DIR)/linker.ld

ELF := $(BUILD_DIR)/$(TARGET).elf
MAP := $(BUILD_DIR)/$(TARGET).map
BIN := $(BUILD_DIR)/$(TARGET).bin
HEX := $(BUILD_DIR)/$(TARGET).hex

FLASH_ADDR ?= 0x08000000
FLASH_TOOL ?= st-flash
ST_FLASH_FLAGS ?= --connect-under-reset --reset
OPENOCD_INTERFACE ?= interface/stlink.cfg
OPENOCD_TARGET ?= target/stm32f4x.cfg
OPENOCD_TRANSPORT ?=
OPENOCD_ADAPTER_SPEED ?=
STM32_PROGRAMMER_CLI ?= STM32_Programmer_CLI
OPENOCD_CFG ?= .vscode/openocd-stm32f401re-stlink.cfg
JLINK ?= JLinkExe
JLINK_DEVICE ?= STM32F401RE
JLINK_IF ?= SWD
JLINK_SPEED ?= 4000
JLINK_SCRIPT ?= $(BUILD_DIR)/flash.jlink
QEMU_SYSTEM_ARM ?= qemu-system-arm
QEMU_M4_MACHINE ?= mps2-an386
QEMU_M4_CPU ?= cortex-m4
QEMU_M33_MACHINE ?= mps2-an505
QEMU_M33_CPU ?= cortex-m33
QEMU_EXTRA_FLAGS ?=
QEMU_GDB_PORT ?= 3333
QEMU_DEBUG_BUILD ?= qemu-debug
QEMU_DEBUG_OPT ?= -Og
QEMU_M4_PID_FILE ?= $(BUILD_DIR)/qemu-m4.pid
QEMU_M4_LOG_FILE ?= $(BUILD_DIR)/qemu-m4.log
QEMU_PID_FILE ?= $(BUILD_DIR)/qemu-m33.pid
QEMU_LOG_FILE ?= $(BUILD_DIR)/qemu-m33.log

BOARD_FLASH_TOOL ?= openocd
BOARD_BUILD_ROOT ?= build-board
BOARD_TIMEOUT ?= 20
BOARD_BAUD ?= 115200
BOARD_LOG_DIR ?= build-board/logs
BOARD_RESET_AFTER_FLASH ?= ON
SERIAL_PORT ?=
MAKE_JOBS ?= 4

INCLUDES := \
    -I$(ARCH_DIR)/kernel/inc \
    -I$(CORE_DIR)/inc \
    -I$(MIDDLEWARE_DIR)/inc \
    -I$(MIDDLEWARE_DIR)/littlefs \
    -I$(APP_DIR)/inc

DEFINES :=
DEFINES += -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_ASSERT -DLFS_NO_INTRINSICS
DEFINES += -DRK_BUILD_COOKIE=$(RK_BUILD_COOKIE)

RK_CONF_FILESYSTEM_FROM_DEFS := $(patsubst -DRK_CONF_FILESYSTEM=%,%,$(filter -DRK_CONF_FILESYSTEM=%,$(EXTRA_DEFINES) $(EXTRA_DEFS)))
ifneq ($(strip $(RK_CONF_FILESYSTEM_FROM_DEFS)),)
RK_CONF_FILESYSTEM_EFFECTIVE := $(lastword $(RK_CONF_FILESYSTEM_FROM_DEFS))
else
ifneq ($(origin RK_CONF_FILESYSTEM),undefined)
RK_CONF_FILESYSTEM_EFFECTIVE := $(RK_CONF_FILESYSTEM)
else ifeq ($(PLATFORM),stm32f401re)
RK_CONF_FILESYSTEM_EFFECTIVE := ON
else
RK_CONF_FILESYSTEM_EFFECTIVE := OFF
endif
endif

ifneq ($(filter $(RK_CONF_FILESYSTEM_EFFECTIVE),ON OFF),$(RK_CONF_FILESYSTEM_EFFECTIVE))
$(error RK_CONF_FILESYSTEM must be ON or OFF)
endif

RK_CONF_FILESYSTEM_RECURSE :=
ifeq ($(strip $(RK_CONF_FILESYSTEM_FROM_DEFS)),)
ifneq ($(origin RK_CONF_FILESYSTEM),undefined)
RK_CONF_FILESYSTEM_RECURSE := RK_CONF_FILESYSTEM='$(RK_CONF_FILESYSTEM)'
endif
endif

ifneq ($(filter $(ARCH),armv7m armv8m),$(ARCH))
$(error Unsupported ARCH '$(ARCH)': RK01 supports MPU hardware on ARCH=armv7m or ARCH=armv8m)
endif

ifeq ($(PLATFORM),stm32f401re)
ifneq ($(ARCH),armv7m)
$(error PLATFORM '$(PLATFORM)' requires ARCH=armv7m)
endif
CPU_FLAGS ?= -mcpu=cortex-m4 -mthumb
DEFINES += -DSTM32F401xE -DRK_MCU_F401RE
DEFINES += -D__NVIC_PRIO_BITS=4
ifeq ($(FPU),ON)
FPU_FLAGS ?= -mfpu=fpv4-sp-d16 -mfloat-abi=hard
DEFINES += -D__FPU_PRESENT=1 -DRK_CONF_FPU=ON
else ifeq ($(FPU),OFF)
FPU_FLAGS ?= -mfloat-abi=soft
DEFINES += -D__FPU_PRESENT=0 -DRK_CONF_FPU=OFF
else
$(error Unsupported FPU '$(FPU)': use FPU=ON or FPU=OFF)
endif
else ifeq ($(PLATFORM),mps2-an505)
ifneq ($(ARCH),armv8m)
$(error PLATFORM '$(PLATFORM)' requires ARCH=armv8m)
endif
ifneq ($(FPU),OFF)
$(error PLATFORM '$(PLATFORM)' currently supports FPU=OFF only)
endif
CPU_FLAGS ?= -mcpu=cortex-m33 -mthumb
FPU_FLAGS ?= -mfloat-abi=soft
DEFINES += -DRK_MCU_MPS2_AN505 -D__FPU_PRESENT=0 -DRK_CONF_FPU=OFF -D__NVIC_PRIO_BITS=3
else ifeq ($(PLATFORM),mps2-an386)
ifneq ($(ARCH),armv7m)
$(error PLATFORM '$(PLATFORM)' requires ARCH=armv7m)
endif
ifneq ($(FPU),OFF)
$(error PLATFORM '$(PLATFORM)' currently supports FPU=OFF only)
endif
ifeq ($(LINKER_SCRIPT),$(ARCH_DIR)/linker.ld)
LINKER_SCRIPT := $(ARCH_DIR)/linker_mps2_an386.ld
endif
CPU_FLAGS ?= -mcpu=cortex-m4 -mthumb
FPU_FLAGS ?= -mfloat-abi=soft
DEFINES += -DRK_MCU_MPS2_AN386 -D__FPU_PRESENT=0 -DRK_CONF_FPU=OFF -D__NVIC_PRIO_BITS=3
else
$(error Unsupported PLATFORM '$(PLATFORM)': RK01 supports stm32f401re, mps2-an386 and mps2-an505 MPU targets)
endif

ifeq ($(RK_CONF_FILESYSTEM_EFFECTIVE),ON)
ifneq ($(PLATFORM),stm32f401re)
$(error RK_CONF_FILESYSTEM=ON is currently supported only on PLATFORM=stm32f401re)
endif
MIDDLEWARE_SRCS += \
    $(MIDDLEWARE_DIR)/src/rkfs.c \
    $(MIDDLEWARE_DIR)/littlefs/lfs.c \
    $(MIDDLEWARE_DIR)/littlefs/lfs_util.c
endif

ifeq ($(strip $(RK_CONF_FILESYSTEM_FROM_DEFS)),)
DEFINES += -DRK_CONF_FILESYSTEM=$(RK_CONF_FILESYSTEM_EFFECTIVE)
endif

DEFINES += $(EXTRA_DEFINES) $(EXTRA_DEFS) $(APP_DEFS)

C_SRCS := \
    $(filter-out $(CORE_DIR)/src/ktrace.c,$(wildcard $(CORE_DIR)/src/*.c)) \
    $(MIDDLEWARE_SRCS) \
    $(wildcard $(ARCH_DIR)/kernel/src/*.c) \
    $(APP_SRCS)

ASM_SRCS := \
    $(wildcard $(ARCH_DIR)/kernel/src/*.S)

OBJS := \
    $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS)) \
    $(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SRCS))

DOMAIN_IMPL_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(DOMAIN_IMPL_SRCS))

DEPS := $(OBJS:.o=.d)

OPT ?= -O2

COMMON_FLAGS := \
    $(CPU_FLAGS) \
    $(FPU_FLAGS) \
    $(INCLUDES) \
    $(DEFINES)

CFLAGS ?=
CFLAGS += \
    $(COMMON_FLAGS) \
    -std=c11 \
    $(OPT) \
    -g3 \
    -Wall \
    -Wextra \
    -Wsign-compare \
    -Wsign-conversion \
    -pedantic \
    -Werror \
    -ffunction-sections \
    -fdata-sections \
    -fstack-usage \
    -fno-common

ASFLAGS ?=
ASFLAGS += \
    $(COMMON_FLAGS) \
    -x assembler-with-cpp -g

LDFLAGS ?=
LDFLAGS += \
    $(CPU_FLAGS) \
    $(FPU_FLAGS) \
    -nostartfiles \
    -T $(LINKER_SCRIPT) \
    -Wl,-Map=$(MAP),--cref \
    -Wl,--gc-sections \
    -specs=nano.specs

LDLIBS ?= -lc

.PHONY: all help clean size objects compile_commands audit-domain-writable flash qemu-m4 qemu-m4-debug qemu-m4-debug-start qemu-m4-debug-stop run-qemu-m4 run-qemu-m4-debug run-qemu-m4-debug-start run-qemu-m4-debug-stop qemu-m33 qemu-m33-debug qemu-m33-debug-start qemu-m33-debug-stop run-qemu-m33 run-qemu-m33-debug run-qemu-m33-debug-start run-qemu-m33-debug-stop board-run FORCE

all: $(ELF) $(BIN) $(HEX) size

ifneq ($(AUDIT_DOMAIN_WRITABLE),OFF)
all: audit-domain-writable
endif

help:
	@printf '%s\n' \
		    'RK01 firmware Makefile help' \
	    '' \
	    'Targets:' \
	    '  make                         Build ELF/BIN/HEX for the current platform.' \
	    '  make objects                 Compile objects only.' \
	    '  make compile_commands        Generate compile_commands.json for the current app profile.' \
	    '  make audit-domain-writable   Warn on writable globals in DOMAIN_IMPL_SRCS.' \
	    '  make size                    Print section sizes for the ELF.' \
	    '  make flash APP_EXAMPLE=name  Build and flash an explicit STM32 app profile.' \
	    '  make qemu-m4                 Build and run the Cortex-M4 MPS2 AN386 QEMU target.' \
	    '  make qemu-m4-debug           Run QEMU M4 halted on the GDB port.' \
	    '  make qemu-m4-debug-start     Start halted QEMU M4 in the background for VS Code.' \
	    '  make qemu-m4-debug-stop      Stop the background QEMU M4 debug server.' \
	    '  make qemu-m33                Build and run the Cortex-M33 MPS2 AN505 QEMU target.' \
	    '  make qemu-m33-debug          Run QEMU M33 halted on the GDB port.' \
	    '  make qemu-m33-debug-start    Start halted QEMU M33 in the background for VS Code.' \
	    '  make qemu-m33-debug-stop     Stop the background QEMU M33 debug server.' \
	    '  make clean                   Remove all build output directories.' \
	    '  make board-run               Build, flash and capture a bounded board log.' \
	    '  make help                    Show this help.' \
	    '' \
	    'Core variables:' \
	    '  ARCH=armv7m|armv8m           CPU architecture. Default: armv7m.' \
	    '  PLATFORM=stm32f401re         Cortex-M4 MPU board target. Default.' \
	    '  PLATFORM=mps2-an386          Cortex-M4 MPU QEMU target.' \
	    '  PLATFORM=mps2-an505          Cortex-M33 MPU QEMU target.' \
		    '  TARGET=name                  Output basename. Default: rk01_demo.' \
	    '  APP_EXAMPLE=name             Example profile. Default: tiny.' \
	    '  BUILD=name                   Build profile directory label. Default: debug.' \
	    '  FPU=ON|OFF                   Enable F401 M4F hard-float context support. Default: OFF.' \
	    '  RK_CONF_FILESYSTEM=ON|OFF    Include RKFS/LittleFS. Default: ON for F401, OFF elsewhere.' \
	    '  BUILD_ROOT=dir               Root output directory. Default: build.' \
	    '  APP_SRCS=files               Override application source list.' \
	    '  DOMAIN_IMPL_SRCS=files       Domain bundle sources audited for writable globals.' \
	    '  AUDIT_DOMAIN_WRITABLE=OFF    Disable the domain writable-state audit.' \
	    '  EXTRA_DEFINES=defs           Append compiler defines.' \
	    '  OPT=-O0|-Og|-O2              Optimisation level. Default: -O2.' \
	    '  FLASH_TOOL=tool              st-flash, openocd, stm32programmer, jlink.' \
	    '  OPENOCD_CFG=file             OpenOCD config for FLASH_TOOL=openocd.' \
	    '  JLINK=path                   J-Link Commander binary. Default: JLinkExe.' \
	    '  JLINK_DEVICE=device          J-Link device name. Default: STM32F401RE.' \
	    '  JLINK_IF=SWD|JTAG            J-Link target interface. Default: SWD.' \
	    '  JLINK_SPEED=kHz|auto         J-Link target speed. Default: 4000.' \
	    '  BOARD_BUILD_ROOT=dir         board-run output root. Default: build-board.' \
	    '  BOARD_FLASH_TOOL=tool        board-run flash tool. Default: openocd.' \
	    '  SERIAL_PORT=dev              UART device for board-run log capture.' \
	    '  BOARD_TIMEOUT=seconds        board-run capture duration. Default: 20.' \
	    '  BOARD_BAUD=baud              board-run UART baud. Default: 115200.' \
	    '  BOARD_RESET_AFTER_FLASH=ON|OFF Reset/run after serial capture starts. Default: ON.' \
	    '  QEMU_SYSTEM_ARM=path         QEMU binary for make qemu-m4/qemu-m33. Default: qemu-system-arm.' \
	    '  QEMU_EXTRA_FLAGS=flags       Extra flags appended to the QEMU command.' \
	    '  QEMU_GDB_PORT=port           GDB port for QEMU debug. Default: 3333.' \
	    '  QEMU_DEBUG_BUILD=name        Build profile for QEMU debug. Default: qemu-debug.' \
	    '  QEMU_DEBUG_OPT=-Og|-O0       Optimisation for QEMU debug. Default: -Og.' \
	    '  TOOLCHAIN=prefix             Toolchain prefix. Default: arm-none-eabi.' \
	    '' \
	    'Examples:' \
	    '  make -j4 ARCH=armv7m PLATFORM=stm32f401re' \
	    '  make -j4 APP_EXAMPLE=01-fleet' \
	    '  make -j4 APP_EXAMPLE=02-isolated' \
	    '  make -j4 APP_EXAMPLE=03-watchdog' \
	    '  make -j4 APP_EXAMPLE=04-sysmon' \
	    '  make -j4 APP_EXAMPLE=05-profile-preempt EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000"' \
	    '  make -j4 APP_EXAMPLE=05-profile-preempt EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000 -DPROFILE_PREEMPT_CLASS=PROFILE_PREEMPT_CLASS_PER_TASK_DOMAIN"' \
	    '  make -j4 APP_EXAMPLE=06-profile-ctxsw EXTRA_DEFS="-DNDEBUG"' \
	    '  make -j4 APP_EXAMPLE=06-profile-ctxsw EXTRA_DEFS="-DNDEBUG -DPROFILE_CTXSW_CLASS=PROFILE_CTXSW_CLASS_INTER_DOMAIN"' \
	    '  make -j4 APP_EXAMPLE=07-signals' \
	    '  make -j4 APP_EXAMPLE=reg-signal-synch' \
	    '  make -j4 APP_EXAMPLE=99-showcase' \
	    '  make -j4 ARCH=armv7m PLATFORM=stm32f401re FPU=ON' \
	    '  make -j4 ARCH=armv7m PLATFORM=mps2-an386' \
	    '  make -j4 ARCH=armv8m PLATFORM=mps2-an505' \
	    '  make qemu-m4' \
	    '  make qemu-m33' \
	    '  make qemu-m33-debug' \
	    'Flash examples:' \
	    '  make flash PLATFORM=stm32f401re APP_EXAMPLE=tiny' \
	    '  make flash PLATFORM=stm32f401re APP_EXAMPLE=04-sysmon FLASH_TOOL=openocd' \
	    '  make flash PLATFORM=stm32f401re APP_EXAMPLE=03-watchdog FLASH_TOOL=stm32programmer' \
	    '  make flash PLATFORM=stm32f401re APP_EXAMPLE=99-showcase FLASH_TOOL=jlink' \
	    'Board harness examples:' \
	    '  make board-run SERIAL_PORT=/dev/cu.usbmodemXXXX'

objects: $(OBJS)

compile_commands:
	@tmp="compile_commands.json.tmp"; \
	printf '[\n' > "$$tmp"; \
	first=1; \
	for src in $(C_SRCS); do \
	    obj="$(BUILD_DIR)/$${src%.c}.o"; \
	    dep="$${obj%.o}.d"; \
	    flags="$(CFLAGS)"; \
	    case "$$src" in \
	        $(MIDDLEWARE_DIR)/littlefs/*.c) \
	            flags="$$flags -Wno-error=sign-conversion -Wno-error=sign-compare";; \
	    esac; \
	    if [ "$$first" -eq 0 ]; then printf ',\n' >> "$$tmp"; fi; \
	    first=0; \
	    printf '  {\n' >> "$$tmp"; \
	    printf '    "directory": "%s",\n' '$(CURDIR)' >> "$$tmp"; \
	    printf '    "file": "%s",\n' "$$src" >> "$$tmp"; \
	    printf '    "output": "%s",\n' "$$obj" >> "$$tmp"; \
	    printf '    "command": "%s %s -MMD -MP -MF %s -c %s -o %s"\n' \
	        '$(CC)' "$$flags" "$$dep" "$$src" "$$obj" >> "$$tmp"; \
	    printf '  }' >> "$$tmp"; \
	done; \
	printf '\n]\n' >> "$$tmp"; \
	mv "$$tmp" compile_commands.json

audit-domain-writable: $(DOMAIN_IMPL_OBJS)
	@if [ -n "$(strip $(DOMAIN_IMPL_OBJS))" ]; then \
	    OBJDUMP="$(OBJDUMP)" sh tools/audit_domain_writable.sh $(DOMAIN_IMPL_OBJS) || true; \
	fi

qemu-m33:
	$(MAKE) ARCH=armv8m PLATFORM=mps2-an505 BUILD_ROOT=$(BUILD_ROOT) $(RK_CONF_FILESYSTEM_RECURSE) APP_EXAMPLE='$(APP_EXAMPLE)' EXTRA_DEFINES='$(EXTRA_DEFINES)' EXTRA_DEFS='$(EXTRA_DEFS)' run-qemu-m33

qemu-m33-debug:
	$(MAKE) ARCH=armv8m PLATFORM=mps2-an505 BUILD_ROOT=$(BUILD_ROOT) BUILD=$(QEMU_DEBUG_BUILD) TARGET=$(TARGET) $(RK_CONF_FILESYSTEM_RECURSE) APP_EXAMPLE='$(APP_EXAMPLE)' EXTRA_DEFINES='$(EXTRA_DEFINES)' EXTRA_DEFS='$(EXTRA_DEFS)' OPT='$(QEMU_DEBUG_OPT)' QEMU_GDB_PORT=$(QEMU_GDB_PORT) run-qemu-m33-debug

qemu-m33-debug-start:
	$(MAKE) ARCH=armv8m PLATFORM=mps2-an505 BUILD_ROOT=$(BUILD_ROOT) BUILD=$(QEMU_DEBUG_BUILD) TARGET=$(TARGET) $(RK_CONF_FILESYSTEM_RECURSE) APP_EXAMPLE='$(APP_EXAMPLE)' EXTRA_DEFINES='$(EXTRA_DEFINES)' EXTRA_DEFS='$(EXTRA_DEFS)' OPT='$(QEMU_DEBUG_OPT)' QEMU_GDB_PORT=$(QEMU_GDB_PORT) run-qemu-m33-debug-start

qemu-m33-debug-stop:
	$(MAKE) ARCH=armv8m PLATFORM=mps2-an505 BUILD_ROOT=$(BUILD_ROOT) BUILD=$(QEMU_DEBUG_BUILD) TARGET=$(TARGET) $(RK_CONF_FILESYSTEM_RECURSE) APP_EXAMPLE='$(APP_EXAMPLE)' QEMU_GDB_PORT=$(QEMU_GDB_PORT) run-qemu-m33-debug-stop

qemu-m4:
	$(MAKE) ARCH=armv7m PLATFORM=mps2-an386 BUILD_ROOT=$(BUILD_ROOT) $(RK_CONF_FILESYSTEM_RECURSE) APP_EXAMPLE='$(APP_EXAMPLE)' EXTRA_DEFINES='$(EXTRA_DEFINES)' EXTRA_DEFS='$(EXTRA_DEFS)' run-qemu-m4

qemu-m4-debug:
	$(MAKE) ARCH=armv7m PLATFORM=mps2-an386 BUILD_ROOT=$(BUILD_ROOT) BUILD=$(QEMU_DEBUG_BUILD) TARGET=$(TARGET) $(RK_CONF_FILESYSTEM_RECURSE) APP_EXAMPLE='$(APP_EXAMPLE)' EXTRA_DEFINES='$(EXTRA_DEFINES)' EXTRA_DEFS='$(EXTRA_DEFS)' OPT='$(QEMU_DEBUG_OPT)' QEMU_GDB_PORT=$(QEMU_GDB_PORT) run-qemu-m4-debug

qemu-m4-debug-start:
	$(MAKE) ARCH=armv7m PLATFORM=mps2-an386 BUILD_ROOT=$(BUILD_ROOT) BUILD=$(QEMU_DEBUG_BUILD) TARGET=$(TARGET) $(RK_CONF_FILESYSTEM_RECURSE) APP_EXAMPLE='$(APP_EXAMPLE)' EXTRA_DEFINES='$(EXTRA_DEFINES)' EXTRA_DEFS='$(EXTRA_DEFS)' OPT='$(QEMU_DEBUG_OPT)' QEMU_GDB_PORT=$(QEMU_GDB_PORT) QEMU_PID_FILE='$(QEMU_M4_PID_FILE)' QEMU_LOG_FILE='$(QEMU_M4_LOG_FILE)' run-qemu-m4-debug-start

qemu-m4-debug-stop:
	$(MAKE) ARCH=armv7m PLATFORM=mps2-an386 BUILD_ROOT=$(BUILD_ROOT) BUILD=$(QEMU_DEBUG_BUILD) TARGET=$(TARGET) $(RK_CONF_FILESYSTEM_RECURSE) APP_EXAMPLE='$(APP_EXAMPLE)' QEMU_GDB_PORT=$(QEMU_GDB_PORT) QEMU_PID_FILE='$(QEMU_M4_PID_FILE)' QEMU_LOG_FILE='$(QEMU_M4_LOG_FILE)' run-qemu-m4-debug-stop

run-qemu-m33: $(ELF)
	$(QEMU_SYSTEM_ARM) -M $(QEMU_M33_MACHINE) -cpu $(QEMU_M33_CPU) -kernel $(ELF) -nographic -serial mon:stdio $(QEMU_EXTRA_FLAGS)

run-qemu-m33-debug: $(ELF)
	$(QEMU_SYSTEM_ARM) -M $(QEMU_M33_MACHINE) -cpu $(QEMU_M33_CPU) -kernel $(ELF) -nographic -serial mon:stdio -S -gdb tcp::$(QEMU_GDB_PORT) $(QEMU_EXTRA_FLAGS)

run-qemu-m33-debug-start: $(ELF)
	@mkdir -p "$(BUILD_DIR)"
	@if [ -f "$(QEMU_PID_FILE)" ]; then \
	    pid=$$(cat "$(QEMU_PID_FILE)"); \
	    if kill -0 "$$pid" 2>/dev/null; then \
	        printf '%s\n' "Stopping stale QEMU M33 pid=$$pid"; \
	        kill "$$pid"; \
	        sleep 0.2; \
	    fi; \
	    rm -f "$(QEMU_PID_FILE)"; \
	fi
	@printf '%s\n' "Starting QEMU M33 GDB server on localhost:$(QEMU_GDB_PORT)"
	@nohup $(QEMU_SYSTEM_ARM) -M $(QEMU_M33_MACHINE) -cpu $(QEMU_M33_CPU) -kernel $(ELF) -nographic -serial mon:stdio -S -gdb tcp::$(QEMU_GDB_PORT) $(QEMU_EXTRA_FLAGS) > "$(QEMU_LOG_FILE)" 2>&1 < /dev/null & echo $$! > "$(QEMU_PID_FILE)"
	@sleep 0.2; \
	if ! kill -0 "$$(cat "$(QEMU_PID_FILE)")" 2>/dev/null; then \
	    printf '%s\n' "QEMU failed to start. Log follows:"; \
	    cat "$(QEMU_LOG_FILE)"; \
	    exit 1; \
	fi; \
	printf '%s\n' "QEMU M33 pid=$$(cat "$(QEMU_PID_FILE)") log=$(QEMU_LOG_FILE)"

run-qemu-m33-debug-stop:
	@if [ -f "$(QEMU_PID_FILE)" ]; then \
	    pid=$$(cat "$(QEMU_PID_FILE)"); \
	    if kill -0 "$$pid" 2>/dev/null; then \
	        printf '%s\n' "Stopping QEMU M33 pid=$$pid"; \
	        kill "$$pid"; \
	    else \
	        printf '%s\n' "QEMU M33 pid=$$pid is not running"; \
	    fi; \
	    rm -f "$(QEMU_PID_FILE)"; \
	else \
	    printf '%s\n' "No QEMU M33 pid file at $(QEMU_PID_FILE)"; \
	fi

run-qemu-m4: $(ELF)
	$(QEMU_SYSTEM_ARM) -M $(QEMU_M4_MACHINE) -cpu $(QEMU_M4_CPU) -kernel $(ELF) -nographic -serial mon:stdio $(QEMU_EXTRA_FLAGS)

run-qemu-m4-debug: $(ELF)
	$(QEMU_SYSTEM_ARM) -M $(QEMU_M4_MACHINE) -cpu $(QEMU_M4_CPU) -kernel $(ELF) -nographic -serial mon:stdio -S -gdb tcp::$(QEMU_GDB_PORT) $(QEMU_EXTRA_FLAGS)

run-qemu-m4-debug-start: $(ELF)
	@mkdir -p "$(BUILD_DIR)"
	@if [ -f "$(QEMU_PID_FILE)" ]; then \
	    pid=$$(cat "$(QEMU_PID_FILE)"); \
	    if kill -0 "$$pid" 2>/dev/null; then \
	        printf '%s\n' "Stopping stale QEMU M4 pid=$$pid"; \
	        kill "$$pid"; \
	        sleep 0.2; \
	    fi; \
	    rm -f "$(QEMU_PID_FILE)"; \
	fi
	@printf '%s\n' "Starting QEMU M4 GDB server on localhost:$(QEMU_GDB_PORT)"
	@nohup $(QEMU_SYSTEM_ARM) -M $(QEMU_M4_MACHINE) -cpu $(QEMU_M4_CPU) -kernel $(ELF) -nographic -serial mon:stdio -S -gdb tcp::$(QEMU_GDB_PORT) $(QEMU_EXTRA_FLAGS) > "$(QEMU_LOG_FILE)" 2>&1 < /dev/null & echo $$! > "$(QEMU_PID_FILE)"
	@sleep 0.2; \
	if ! kill -0 "$$(cat "$(QEMU_PID_FILE)")" 2>/dev/null; then \
	    printf '%s\n' "QEMU failed to start. Log follows:"; \
	    cat "$(QEMU_LOG_FILE)"; \
	    exit 1; \
	fi; \
	printf '%s\n' "QEMU M4 pid=$$(cat "$(QEMU_PID_FILE)") log=$(QEMU_LOG_FILE)"

run-qemu-m4-debug-stop:
	@if [ -f "$(QEMU_PID_FILE)" ]; then \
	    pid=$$(cat "$(QEMU_PID_FILE)"); \
	    if kill -0 "$$pid" 2>/dev/null; then \
	        printf '%s\n' "Stopping QEMU M4 pid=$$pid"; \
	        kill "$$pid"; \
	    else \
	        printf '%s\n' "QEMU M4 pid=$$pid is not running"; \
	    fi; \
	    rm -f "$(QEMU_PID_FILE)"; \
	else \
	    printf '%s\n' "No QEMU M4 pid file at $(QEMU_PID_FILE)"; \
	fi

board-run:
	ARCH=armv7m \
	PLATFORM=stm32f401re \
	BUILD_ROOT='$(BOARD_BUILD_ROOT)' \
	APP_EXAMPLE='$(APP_EXAMPLE)' \
	RK_CONF_FILESYSTEM='$(RK_CONF_FILESYSTEM_EFFECTIVE)' \
	EXTRA_DEFINES='$(EXTRA_DEFINES)' \
	EXTRA_DEFS='$(EXTRA_DEFS)' \
	FLASH_TOOL='$(BOARD_FLASH_TOOL)' \
	TOOLCHAIN='$(TOOLCHAIN)' \
	OPENOCD_CFG='$(OPENOCD_CFG)' \
	JLINK='$(JLINK)' \
	JLINK_DEVICE='$(JLINK_DEVICE)' \
	JLINK_IF='$(JLINK_IF)' \
	JLINK_SPEED='$(JLINK_SPEED)' \
	SERIAL_PORT='$(SERIAL_PORT)' \
	BOARD_TIMEOUT='$(BOARD_TIMEOUT)' \
	BOARD_BAUD='$(BOARD_BAUD)' \
	BOARD_LOG_DIR='$(BOARD_LOG_DIR)' \
	BOARD_RESET_AFTER_FLASH='$(BOARD_RESET_AFTER_FLASH)' \
	MAKE_JOBS='$(MAKE_JOBS)' \
	sh tools/board_harness.sh

$(CONFIG_STAMP): FORCE
	@mkdir -p $(dir $@)
	@tmp="$@.tmp"; \
	{ \
	    printf '%s\n' 'CC=$(CC)'; \
	    printf '%s\n' 'ARCH=$(ARCH)'; \
	    printf '%s\n' 'PLATFORM=$(PLATFORM)'; \
	    printf '%s\n' 'RK_CONF_FILESYSTEM=$(RK_CONF_FILESYSTEM_EFFECTIVE)'; \
	    printf '%s\n' 'BUILD_PROFILE=$(BUILD_PROFILE)'; \
	    printf '%s\n' 'TARGET=$(TARGET)'; \
	    printf '%s\n' 'APP_EXAMPLE=$(APP_EXAMPLE)'; \
	    printf '%s\n' 'APP_SRCS=$(APP_SRCS)'; \
	    printf '%s\n' 'CFLAGS=$(CFLAGS)'; \
	    printf '%s\n' 'ASFLAGS=$(ASFLAGS)'; \
	    printf '%s\n' 'LDFLAGS=$(LDFLAGS)'; \
	    printf '%s\n' 'LDLIBS=$(LDLIBS)'; \
	} > "$$tmp"; \
	if ! cmp -s "$$tmp" "$@"; then mv "$$tmp" "$@"; else rm -f "$$tmp"; fi

$(ELF): $(OBJS) $(LINKER_SCRIPT) $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex $< $@

size: $(ELF)
	$(SIZE) -A $<

ifneq ($(filter $(PLATFORM),stm32f401re),)
flash: require-flash-app-example $(ELF) $(BIN)
	@printf '%s\n' "Flashing APP_EXAMPLE=$(APP_EXAMPLE) image=$(ELF)"
ifeq ($(FLASH_TOOL),st-flash)
	st-flash $(ST_FLASH_FLAGS) write $(BIN) $(FLASH_ADDR)
else ifeq ($(FLASH_TOOL),openocd)
	openocd $(if $(OPENOCD_CFG),-f $(OPENOCD_CFG),-f $(OPENOCD_INTERFACE) $(OPENOCD_TRANSPORT) -f $(OPENOCD_TARGET)) $(if $(OPENOCD_ADAPTER_SPEED),-c "adapter speed $(OPENOCD_ADAPTER_SPEED)") -c "program $(ELF) verify reset exit"
else ifeq ($(FLASH_TOOL),stm32programmer)
	$(STM32_PROGRAMMER_CLI) -c port=SWD -w $(BIN) $(FLASH_ADDR) -v -rst
else ifeq ($(FLASH_TOOL),jlink)
	@mkdir -p "$(dir $(JLINK_SCRIPT))"
	@{ \
	    printf '%s\n' 'r'; \
	    printf '%s\n' 'h'; \
	    printf 'loadbin %s, %s\n' '$(BIN)' '$(FLASH_ADDR)'; \
	    printf 'verifybin %s, %s\n' '$(BIN)' '$(FLASH_ADDR)'; \
	    printf '%s\n' 'r'; \
	    printf '%s\n' 'g'; \
	    printf '%s\n' 'q'; \
	} > "$(JLINK_SCRIPT)"
	$(JLINK) -device "$(JLINK_DEVICE)" -if "$(JLINK_IF)" -speed "$(JLINK_SPEED)" -autoconnect 1 -CommanderScript "$(JLINK_SCRIPT)"
else
	$(error Unsupported FLASH_TOOL '$(FLASH_TOOL)')
endif

require-flash-app-example:
ifeq ($(APP_EXAMPLE_ORIGIN),undefined)
	$(error flash requires APP_EXAMPLE=<name>; plain 'make flash' would default to APP_EXAMPLE=tiny)
else
	@:
endif
else
flash:
	$(error flash is only supported for STM32 board platforms)
endif

$(BUILD_DIR)/$(MIDDLEWARE_DIR)/littlefs/%.o: $(MIDDLEWARE_DIR)/littlefs/%.c Makefile $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-error=sign-conversion -Wno-error=sign-compare -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR)/%.o: %.c Makefile $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR)/%.o: %.S Makefile $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

clean:
	rm -rf build build-*

-include $(DEPS)
