# RK01 - Version 0.1.0

RK01 is the RK0 kernel line with a Cortex-M MPU and an explicit kernel/user
boundary.

RK0 remains a flat real-time executive, with the real-time benefits and the
corruption/fault-containment drawbacks that come with that model.

RK01 keeps the RK0 real-time model, but changes what happens when ordinary task
code is wrong: task code runs unprivileged, kernel services are reached through
system calls, and domain-owned writable state is enforced by the MPU.

This adds substantial complexity to the design and to the analysis. The gain is
that bounded memory regions can confine a fault. Recovering from that fault is
still application-specific.

This repository is the first public RK01 source drop.

For a desin over view read the 
[Blog](https://rkernel0.org/rk01-user-kernel-and-memory-domain-boundaries/).

For a detailed view: [Architecure Specification](docs/asd/asd.mdit )

## Delivered Supported Targets

Cortex-M4(F)/M7 based MCUs with MPU are considered the most suitable targets for RK01. 
ARMv8M chips are supported but would be underused.
Although Cortex-M0+ chips have MPUs, RK01 does not support ARMv6M.

This repo delivers a build environment to run on Nucleo STM32F401RE M4F, plus
QEMU environments for MPS2 Cortex-M4 and Cortex-M33. RK01 does not use the
_Trusted Environment_ of ARMv8M.

## Repository Layout

| Path | Purpose |
| --- | --- |
| `Makefile` | Firmware build, flash and run entry point. |
| `app/src/application.c`, `app/src/record_domain.*`, `app/src/tiny_*.c` | Default public record-console example with Record as a source-bundled domain. |
| `app/examples/` | Selectable `APP_EXAMPLE` profiles. |
| `arch/armv7m/` | STM32F401RE Cortex-M4 port and MPS2 AN386 Cortex-M4 QEMU port. |
| `arch/armv8m/` | MPS2 AN505 Cortex-M33 port. |
| `core/inc/` | Public and internal kernel headers. |
| `core/src/` | Scheduler, syscalls, objects, IPC and fault handling. |
| `middleware/inc/rkfs.h` | RKFS public interface. |
| `middleware/src/rkfs.c` | RKFS facade over LittleFS on F401 when `RK_CONF_FILESYSTEM=ON`. |
| `middleware/littlefs/` | Vendored LittleFS source subset. |


## Build Requirements

- Unlike RK0, which stays aligned with C99, RK01 requires at least C11. C11
  introduces memory-alignment features that would otherwise be cumbersome or
  infeasible in C99.

Required for firmware builds:

- GNU Make (C11/GNU11)
- `arm-none-eabi-gcc`
- `arm-none-eabi-objcopy`
- `arm-none-eabi-size`
- `qemu-system-arm` for the MPS2 QEMU targets

## Quick Start

Build the default STM32F401RE image:

```sh
make -j4 ARCH=armv7m PLATFORM=stm32f401re
```

Build with the F401 M4F hard-float ABI:

```sh
make -j4 ARCH=armv7m PLATFORM=stm32f401re FPU=ON
```

Build the STM32F401RE image without RKFS/LittleFS:

```sh
make -j4 ARCH=armv7m PLATFORM=stm32f401re RK_CONF_FILESYSTEM=OFF
```

Build the Cortex-M4 QEMU image:

```sh
make -j4 ARCH=armv7m PLATFORM=mps2-an386
```

Run the QEMU M4 smoke target:

```sh
make qemu-m4
```

Build the Cortex-M33 QEMU image:

```sh
make -j4 ARCH=armv8m PLATFORM=mps2-an505
```

Run the QEMU M33 smoke target:

```sh
make qemu-m33
```

Build with source-stepping-friendly optimisation:

```sh
make -j4 ARCH=armv7m PLATFORM=stm32f401re OPT=-Og
make -j4 ARCH=armv7m PLATFORM=mps2-an386 OPT=-Og
make -j4 ARCH=armv8m PLATFORM=mps2-an505 OPT=-Og
```

Build the STM32F401RE preemption profiles with `RK_CONF_SYSTICK_DIV=1000`:

```sh
make -j4 ARCH=armv7m PLATFORM=stm32f401re APP_EXAMPLE=05-profile-preempt EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000"
make -j4 ARCH=armv7m PLATFORM=stm32f401re APP_EXAMPLE=05-profile-preempt EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000 -DPROFILE_PREEMPT_CLASS=PROFILE_PREEMPT_CLASS_PER_TASK_DOMAIN"
```

Run the per-task-domain preemption profile on QEMU M4:

```sh
make qemu-m4 APP_EXAMPLE=05-profile-preempt EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000 -DPROFILE_PREEMPT_CLASS=PROFILE_PREEMPT_CLASS_PER_TASK_DOMAIN"
```

## License

RK01 source files use the Apache-2.0 SPDX identifier. See `LICENSE`.

The `middleware/littlefs` subset is the upstream LittleFS code used by RKFS when
`RK_CONF_FILESYSTEM=ON`; it keeps its own licence file in
`middleware/littlefs/LICENSE.md`.
