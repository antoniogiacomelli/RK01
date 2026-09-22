# RK01 Thread-Metric benchmark entry points for F401RE.
#
# Run from the repository root with:
#   make -f benchmarks/Makefile build-f401re
#   make -f benchmarks/Makefile run-f401re SERIAL_PORT=/dev/ttyACM0

BENCHMARK_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
BENCHMARK_DIR := $(dir $(BENCHMARK_MAKEFILE))
RK01_ROOT ?= $(abspath $(BENCHMARK_DIR)/..)
RK01_TOP_MAKEFILE ?= $(RK01_ROOT)/Makefile

PLATFORM ?= stm32f401re

ifneq ($(filter f401re F401RE stm32f401re STM32F401RE stm32f4014re STM32F4014RE,$(PLATFORM)),)
override PLATFORM := stm32f401re
endif

ifneq ($(filter-out help,$(MAKECMDGOALS)),)
ifneq ($(PLATFORM),stm32f401re)
$(error RK01 singleton-domain Thread-Metric benchmarks currently support only PLATFORM=stm32f401re)
endif
endif

THREAD_METRIC_APP ?= benchmarks/benchmark_batch.c
THREAD_METRIC_BUILD ?= PROFILE
THREAD_METRIC_BUILD_ROOT ?= build/benchmarks/armv7m/stm32f401re/thread_metric
THREAD_METRIC_COMMON_DEFS ?= -DNDEBUG -DRK_CONF_TRACE=OFF -DRK_CONF_N_USRTASKS_MAX=8U -DRK_CONF_SYSTICK_DIV=1000U
THREAD_METRIC_DEFS ?= $(THREAD_METRIC_COMMON_DEFS)

ifneq ($(strip $(DURATION_MS)),)
THREAD_METRIC_DEFS += -DRK_THREAD_METRIC_TEST_DURATION_MS=$(DURATION_MS)UL
endif
ifneq ($(strip $(CYCLES)),)
THREAD_METRIC_DEFS += -DRK_THREAD_METRIC_CYCLES=$(CYCLES)UL
endif

THREAD_METRIC_BENCHES := \
	basic-processing \
	cooperative-scheduling \
	preemptive-scheduling \
	interrupt-processing \
	interrupt-preemption-processing \
	message-processing \
	synchronization-processing \
	memory-allocation

BENCHES ?= $(THREAD_METRIC_BENCHES)
THREAD_METRIC_RUNNER ?= $(BENCHMARK_DIR)/run_benchmark.sh

define BUILD_THREAD_METRIC
	$(MAKE) -C "$(RK01_ROOT)" -f "$(RK01_TOP_MAKEFILE)" -B \
		ARCH=armv7m PLATFORM=stm32f401re BUILD=$(THREAD_METRIC_BUILD) \
		FPU=OFF RK_CONF_FILESYSTEM=OFF AUDIT_DOMAIN_WRITABLE=OFF \
		APP_EXAMPLE=thread-metric \
		APP_SRCS="$(THREAD_METRIC_APP)" \
		BUILD_DIR="$(THREAD_METRIC_BUILD_ROOT)/$(1)" \
		TARGET="rk01_thread_metric_$(1)" \
		EXTRA_DEFS='$(EXTRA_DEFS) $(THREAD_METRIC_DEFS) -DRK_THREAD_METRIC_BENCH=$(2)'
endef

define FLASH_THREAD_METRIC
	$(MAKE) -C "$(RK01_ROOT)" -f "$(RK01_TOP_MAKEFILE)" -B \
		ARCH=armv7m PLATFORM=stm32f401re BUILD=$(THREAD_METRIC_BUILD) \
		FPU=OFF RK_CONF_FILESYSTEM=OFF AUDIT_DOMAIN_WRITABLE=OFF \
		APP_EXAMPLE=thread-metric \
		APP_SRCS="$(THREAD_METRIC_APP)" \
		BUILD_DIR="$(THREAD_METRIC_BUILD_ROOT)/$(1)" \
		TARGET="rk01_thread_metric_$(1)" \
		EXTRA_DEFS='$(EXTRA_DEFS) $(THREAD_METRIC_DEFS) -DRK_THREAD_METRIC_BENCH=$(2)' \
		$(if $(strip $(FLASH_TOOL)),FLASH_TOOL=$(FLASH_TOOL)) \
		flash
endef

BUILD_TARGETS := $(addprefix build-thread-metric-,$(BENCHES))
FLASH_TARGETS := $(addprefix flash-thread-metric-,$(BENCHES))

all: build

build: $(BUILD_TARGETS)

flash: $(FLASH_TARGETS)

run:
	$(THREAD_METRIC_RUNNER) \
		--platform "$(PLATFORM)" \
		$(if $(strip $(SERIAL_PORT)),--port "$(SERIAL_PORT)") \
		$(if $(strip $(DURATION_MS)),--duration-ms "$(DURATION_MS)") \
		$(if $(strip $(CYCLES)),--cycles "$(CYCLES)") \
		$(if $(strip $(TIMEOUT_SEC)),--timeout-sec "$(TIMEOUT_SEC)") \
		$(if $(strip $(SERIAL_BAUD)),--baud "$(SERIAL_BAUD)") \
		$(if $(strip $(LOG_DIR)),--logs "$(LOG_DIR)") \
		$(if $(strip $(FLASH_TOOL)),--flash-tool "$(FLASH_TOOL)") \
		--makefile "$(BENCHMARK_MAKEFILE)" \
		$(BENCHES)

build-f401re:
	$(MAKE) -f "$(BENCHMARK_MAKEFILE)" build PLATFORM=stm32f401re BENCHES='$(BENCHES)' \
		$(if $(strip $(DURATION_MS)),DURATION_MS="$(DURATION_MS)") \
		$(if $(strip $(CYCLES)),CYCLES="$(CYCLES)")

run-f401re:
	$(MAKE) -f "$(BENCHMARK_MAKEFILE)" run PLATFORM=stm32f401re BENCHES='$(BENCHES)' \
		$(if $(strip $(SERIAL_PORT)),SERIAL_PORT="$(SERIAL_PORT)") \
		$(if $(strip $(DURATION_MS)),DURATION_MS="$(DURATION_MS)") \
		$(if $(strip $(CYCLES)),CYCLES="$(CYCLES)") \
		$(if $(strip $(TIMEOUT_SEC)),TIMEOUT_SEC="$(TIMEOUT_SEC)") \
		$(if $(strip $(SERIAL_BAUD)),SERIAL_BAUD="$(SERIAL_BAUD)") \
		$(if $(strip $(LOG_DIR)),LOG_DIR="$(LOG_DIR)") \
		$(if $(strip $(FLASH_TOOL)),FLASH_TOOL="$(FLASH_TOOL)")

build-thread-metric-basic-processing:
	$(call BUILD_THREAD_METRIC,basic_processing,1)

build-thread-metric-cooperative-scheduling:
	$(call BUILD_THREAD_METRIC,cooperative_scheduling,2)

build-thread-metric-preemptive-scheduling:
	$(call BUILD_THREAD_METRIC,preemptive_scheduling,3)

build-thread-metric-interrupt-processing:
	$(call BUILD_THREAD_METRIC,interrupt_processing,4)

build-thread-metric-interrupt-preemption-processing:
	$(call BUILD_THREAD_METRIC,interrupt_preemption_processing,5)

build-thread-metric-message-processing:
	$(call BUILD_THREAD_METRIC,message_processing,6)

build-thread-metric-synchronization-processing:
	$(call BUILD_THREAD_METRIC,synchronization_processing,7)

build-thread-metric-memory-allocation:
	$(call BUILD_THREAD_METRIC,memory_allocation,8)

flash-thread-metric-basic-processing:
	$(call FLASH_THREAD_METRIC,basic_processing,1)

flash-thread-metric-cooperative-scheduling:
	$(call FLASH_THREAD_METRIC,cooperative_scheduling,2)

flash-thread-metric-preemptive-scheduling:
	$(call FLASH_THREAD_METRIC,preemptive_scheduling,3)

flash-thread-metric-interrupt-processing:
	$(call FLASH_THREAD_METRIC,interrupt_processing,4)

flash-thread-metric-interrupt-preemption-processing:
	$(call FLASH_THREAD_METRIC,interrupt_preemption_processing,5)

flash-thread-metric-message-processing:
	$(call FLASH_THREAD_METRIC,message_processing,6)

flash-thread-metric-synchronization-processing:
	$(call FLASH_THREAD_METRIC,synchronization_processing,7)

flash-thread-metric-memory-allocation:
	$(call FLASH_THREAD_METRIC,memory_allocation,8)

help:
	@echo "RK01 F401RE singleton-domain Thread-Metric benchmarks"
	@echo
	@echo "Unix-like host required: Linux, macOS, MSYS2, or WSL with Bash, Python 3, and GNU make."
	@echo "See benchmarks/USAGE.txt for details."
	@echo
	@echo "  make -f benchmarks/Makefile build-f401re"
	@echo "  make -f benchmarks/Makefile run-f401re SERIAL_PORT=<port>"
	@echo
	@echo "Options:"
	@echo "  BENCHES='<bench...>'       select benchmarks; default: all"
	@echo "  DURATION_MS=<ms>           report period compiled into each image"
	@echo "  CYCLES=<n>                 stop after n report periods"
	@echo "  TIMEOUT_SEC=<s>            serial wait timeout per benchmark"
	@echo "  SERIAL_BAUD=<baud>         serial baud rate; default: 115200"
	@echo "  FLASH_TOOL=<tool>          jlink, openocd, st-flash, or stm32programmer"
	@echo "  LOG_DIR=<dir>              summary and per-benchmark logs"
	@echo
	@echo "Benchmarks:"
	@printf "  %s\n" $(THREAD_METRIC_BENCHES)

.PHONY: all build flash run build-f401re run-f401re \
	build-thread-metric-basic-processing \
	build-thread-metric-cooperative-scheduling \
	build-thread-metric-preemptive-scheduling \
	build-thread-metric-interrupt-processing \
	build-thread-metric-interrupt-preemption-processing \
	build-thread-metric-message-processing \
	build-thread-metric-synchronization-processing \
	build-thread-metric-memory-allocation \
	flash-thread-metric-basic-processing \
	flash-thread-metric-cooperative-scheduling \
	flash-thread-metric-preemptive-scheduling \
	flash-thread-metric-interrupt-processing \
	flash-thread-metric-interrupt-preemption-processing \
	flash-thread-metric-message-processing \
	flash-thread-metric-synchronization-processing \
	flash-thread-metric-memory-allocation \
	help
