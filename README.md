# RK01 - Version 0.1.0

RK01 is the RK0 kernel line with a Cortex-M MPU and an explicit kernel/user
boundary.

RK0 remains a flat real-time executive. Not a flaw. 

RK01 keeps the RK0 real-time model, but changes what happens when ordinary task code is wrong: task code runs
unprivileged, kernel services are reached _through a system call, and domain-owned
writable state is enforced by the MPU._

This adds substantial complexity to the design and to the analysis. The gain is with bounded memory regions a
fault can be confined. Still, recovering from that fault is application-specific.

This repository is the first public RK01 source drop. 
Current version: 0.1.0.

## What RK01 Adds To RK0

RK01 is not a better RK0; neither an embedded RTOS trying to be a GPOS. I
t is still one statically linked firmware image for a microcontroller. The kernel, startup
code, board port, privileged service tasks and build are trusted. Ordinary
application tasks are treated as possibly defective after dispatch -- and if it fails, 
it will be confined and then handled the best it can. 

Immediate differences from RK0:

| Area | RK0 | RK01 |
| --- | --- | --- |
| Firmware model | One trusted firmware image. | V0.1.0 still builds one firmware image. Future linker-described domain images are ThreadX-inspired, but no standalone loader is shipped yet. |
| Kernel boundary | Kernel and application code share privileged address space. | Ordinary tasks run unprivileged on PSP and enter SVC for kernel services. |
| Memory protection | Cooperative discipline. | Cortex-M MPU regions protect kernel RAM, domain RAM and shared apertures. |
| Application grouping | Tasks can directly share C globals. | Tasks share memory only inside their domain, global shared RAM or explicit shared memory. |
| Kernel objects | Raw/static objects are natural; pool-backed creation is optional. | Runtime objects are fixed-capacity kernel pool entries addressed by opaque handles. |
| IPC rule | Pointer transfer is fine when firmware agrees. | By-reference direct messages are same-domain only; cross-domain data should be copied. |
| Bad syscall pointers | A bad pointer can become a privileged fault if unchecked. | SVC validates user read/write/function ranges before privileged code dereferences them. |
| Fault handling | Serious task faults usually become system faults. | Unprivileged MemManage faults can be contained, marked `FAULT_PENDING` and cleaned by PostProc. |
| Documentation state | Mature RK0 docs exist outside the source tree. | First public drop uses this README as the primary public guide. |

The short rule is: `RK0` is flat trusted real-time firmware; 
`RK01` is RK0-style real-time with user space/kernel space.
## Architecture Sketch

![RK01 containment sketch](docs/readme_architecture_sketch.svg)

## Containment Scope
Its protection boundary is practical and local:

- ordinary tasks run unprivileged and enter kernel services through SVC;
- kernel RAM, object pools, registries and privileged stacks remain
  privileged-only;
- each domain provides a statically declared writable-authority boundary shared
  by one or more tasks;
- cross-domain data moves through copied IPC, global shared RAM or explicitly
  attached shared memory;
- unprivileged MemManage faults can be recorded, contained and cleaned up by
  PostProc.

The trusted base remains the kernel, startup code, board port, privileged
service tasks, build, DMA setup, debug access and physical device access. RK01
focuses on preventing defective unprivileged task code from corrupting kernel
RAM, another domain's writable state or privileged service state through
ordinary bad-pointer mistakes.

## Delivered Supported Targets

This repo delivers a build environment to run on Nucleo STM32F401RE M4F, and a QEMU environment for MPS2 Cortex-M33. RK01 does not use the _Trusted Environment_ from ARMv8M but this was the QEMU system of choice, and a means to test portability to _ARMv8M_.

| Target | Role | Status |
| --- | --- | --- |
| `ARCH=armv7m PLATFORM=stm32f401re` | STM32F401RE board path. | Main hardware path for timing, UART, flash and MPU behaviour. |
| `ARCH=armv8m PLATFORM=mps2-an505` | QEMU MPS2 AN505 Cortex-M33. | Fast functional smoke path for SVC and MPU mechanics. |

RK01 does not currently ship an ARMv6-M port. A future Cortex-M0+ MPU board can
still be useful, but it should be treated as a smaller single-domain profile,
not as proof that multi-domain isolation is practical on every MCU.

## Repository Layout

| Path | Purpose |
| --- | --- |
| `Makefile` | Firmware build, flash and run entry point. |
| `app/src/application.c`, `app/src/record_domain.*`, `app/src/tiny_*.c` | Default public record-console example with Record as a source-bundled domain. |
| `app/examples/` | Selectable `APP_EXAMPLE` profiles. |
| `arch/armv7m/` | STM32F401RE Cortex-M4 port. |
| `arch/armv8m/` | MPS2 AN505 Cortex-M33 port. |
| `core/inc/` | Public and internal kernel headers. |
| `core/src/` | Scheduler, syscalls, objects, IPC and fault handling. |
| `middleware/inc/rkfs.h` | RKFS public interface. |
| `middleware/src/rkfs.c` | RKFS facade over LittleFS on F401. |
| `middleware/littlefs/` | Vendored LittleFS source subset. |
| `tools/audit_domain_writable.sh` | Optional build audit for unexpected writable globals in explicitly declared domain implementation sources. |
| `tools/board_harness.sh` | Build, flash and serial capture helper. |

The public repo intentionally leaves out local build products, board logs,
private harnesses, scratch scripts, `.DS_Store`, stack-usage files and personal
IDE settings.

## Build Requirements

- Unlike RK0, which conservatively stays aligned with C99, RK01 requires a C11
  compiler.

Required for firmware builds:

- GNU Make
- `arm-none-eabi-gcc`
- `arm-none-eabi-objcopy`
- `arm-none-eabi-size`

Optional:

- `qemu-system-arm` for the Cortex-M33 smoke target
- OpenOCD, `st-flash`, STM32 Programmer CLI or J-Link for STM32F401RE flashing
- a serial terminal, or `make board-run`, for board UART output

The Makefile defaults are `ARCH=armv7m`, `PLATFORM=stm32f401re`,
`APP_EXAMPLE=tiny`, `TARGET=rk01_demo` and `FPU=OFF`.

## Quick Start

Build the default STM32F401RE image:

```sh
make -j4 ARCH=armv7m PLATFORM=stm32f401re
```

Build with the F401 M4F hard-float ABI:

```sh
make -j4 ARCH=armv7m PLATFORM=stm32f401re FPU=ON
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
make -j4 ARCH=armv8m PLATFORM=mps2-an505 OPT=-Og
```

## Default Application

The default `APP_EXAMPLE=tiny` application is a small record console.

On STM32F401RE:

- USART2 on PA2/PA3 receives terminal input.
- A privileged console driver task owns UART RX/TX for normal console traffic.
- Ordinary task writes enter SVC and synchronously call that UART service; the
  service executes the transaction at the caller's effective priority for the
  extended call/reply rendezvous.
- UART RX is interrupt-driven and delivered by the service to the current
  foreground RX owner.
- The foreground app console accepts CR, LF and CRLF endings.
- Completed input lines are stored in a shared line ring, up to
  `RK_CONF_CONSOLE_LINE_MAX_BYTES` bytes.
- `EchoTask` runs unprivileged in an explicit `Echo` domain.
- `EchoTask` parses commands such as `SET A 123` and `READ A`.
- In the default mixed console, `RKMONITOR` enters SysMon diagnostics. Commands
  such as `ps` are then accepted directly until `exit` or `quit`.
- The Record service is a source-bundled `Rec` domain:
  `record_domain.h` exposes copied request/reply types and
  `RecordDomainBoot()`, while `record_domain_internal.h` keeps the private RAM
  layout.
- Echo calls Record through copied call/reply messages.
- A privileged `FS` service task owns RKFS/LittleFS, reserved flash and the
  STM32 flash controller.
- Record slots are persisted in the reserved `FS_FLASH` region.

Expected board banner:

> `RK01 USART2 record console ready. SET A 123, READ A, RKMONITOR.`
> `Record slots: 4, persisted in flash through rkfs.`

Try `SET A 123`, `READ A`, `SET B 77`, `READ B`, `RKMONITOR`, `ps`, `exit`.

The same application shape builds for the MPS2 AN505 target, but without
STM32F401RE flash persistence.

## Example Profiles

Select examples with `APP_EXAMPLE`:

```sh
make -j4 APP_EXAMPLE=01-fleet
make -j4 APP_EXAMPLE=02-isolated
make -j4 APP_EXAMPLE=03-watchdog
make -j4 APP_EXAMPLE=04-sysmon
make -j4 APP_EXAMPLE=05-profile-preempt EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000"
make -j4 APP_EXAMPLE=06-profile-ctxsw EXTRA_DEFS="-DNDEBUG"
make -j4 APP_EXAMPLE=99-showcase
```

| Example | Focus |
| --- | --- |
| `tiny` | Record console, Echo/Record domains, copied call/reply, RKFS on F401. |
| `01-fleet` | Explicit Control/Comms domains and copied task-addressed IPC. |
| `02-isolated` | Isolated one-task domains exchanging copied messages. |
| `03-watchdog` | STM32F401RE watchdog through the HAL, or a heartbeat task on targets without one. |
| `04-sysmon` | Optional SysMon kernel diagnostic service. |
| `05-profile-preempt` | RK0-style ThreadX preemptive scheduling counter profile. |
| `06-profile-ctxsw` | RK0-style same-priority yield context-switch cycle profile. |
| `99-showcase` | Larger combined example with App tasks, isolated tasks, explicit domains and copied IPC. |

## Profiling

The profiling examples mirror the RK0 profiling conditions on the
STM32F401RE: Cortex-M4F at 80 MHz, `-O2`, `-DNDEBUG`, FPU disabled, debug
symbols kept, stack-usage output enabled and stack-overflow checking not
enabled.

Run the preemptive scheduling counter profile with a 1 ms tick:

```sh
make board-run APP_EXAMPLE=05-profile-preempt \
    SERIAL_PORT=/dev/cu.usbmodemXXXX \
    BOARD_TIMEOUT=155 \
    BOARD_FLASH_TOOL=openocd \
    FPU=OFF \
    EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000"
```

Run the context-switch cycle profile with the default 10 ms tick:

```sh
make board-run APP_EXAMPLE=06-profile-ctxsw \
    SERIAL_PORT=/dev/cu.usbmodemXXXX \
    BOARD_TIMEOUT=25 \
    BOARD_FLASH_TOOL=openocd \
    FPU=OFF \
    EXTRA_DEFS="-DNDEBUG"
```

Captured board results from the RK01 STM32F401RE path:

| Profile | Result |
| --- | --- |
| Preemptive scheduling | Five 30 s rounds, `errors=0`, final average `1441372` per task, max task-counter spread `1`. |
| Context switch | Two 10 s rounds, raw PendSV min/last `385` cycles, raw max `409` cycles. With the RK0 `-7` cycle adjustment this is `378..402` cycles, about `4.7..5.0 us` at 80 MHz. |

## Flashing And Board Capture

Flash with the selected tool:

```sh
make flash ARCH=armv7m PLATFORM=stm32f401re FLASH_TOOL=openocd
make flash ARCH=armv7m PLATFORM=stm32f401re FLASH_TOOL=st-flash
make flash ARCH=armv7m PLATFORM=stm32f401re FLASH_TOOL=stm32programmer
make flash ARCH=armv7m PLATFORM=stm32f401re FLASH_TOOL=jlink
```

Run the board harness:

```sh
make board-run SERIAL_PORT=/dev/cu.usbmodemXXXX
make board-run BOARD_TIMEOUT=10
make board-run BOARD_FLASH_TOOL=jlink SERIAL_PORT=/dev/cu.usbmodemXXXX
```

The harness builds, flashes, resets and captures a bounded UART log into
`build-board/logs/`.

## Execution Model

Startup remains simple: `Reset_Handler` enters `main()`, which calls
`kCoreInit()` and then `kInit()`. Kernel init sets up fixed object pools,
privileged system tasks, application BOOT construction, MPU layout finalisation
and the first task dispatch. Ordinary task code then runs unprivileged and uses
SVC for kernel services.

![RK01 startup and first dispatch](docs/readme_execution_model.svg)

Handlers run privileged on MSP. Ordinary tasks run on PSP and normally run
unprivileged. Privileged system tasks also use PSP, but keep privileged CONTROL
state.

## Memory Model

RK01 separates memory by privilege first and by domain ownership second. A
task's effective MPU configuration combines its domain, private stack and
explicitly granted shared-memory or peripheral regions.

![STM32F401RE memory domains](docs/readme_memory_model.svg)

| Region | Access |
| --- | --- |
| Flash | User-readable and executable. On STM32F401RE this is `0x08000000..0x08040000`. |
| FS_FLASH | STM32F401RE reserved flash at `0x08040000..0x08080000`, used by RKFS. Not user executable. |
| Task/domain RAM | Writable only by tasks whose current MPU view maps that domain. |
| Global shared RAM | Small firmware-wide aperture mapped into ordinary tasks. |
| Explicit shared memory | Boot-created shared segment attached only to selected domains. |
| Kernel RAM | Privileged only. Contains TCBs, object pools, registries and privileged stacks. |

Typical MPU slot intent:

| MPU slot | Meaning |
| --- | --- |
| Region 0 | User-readable executable Flash, installed once. |
| Region 1 | Active task/domain RAM, rewritten on context switch. |
| Region 2 | Global shared RAM, rewritten from the incoming TCB. |
| Regions 3..7 | Explicit shared-memory segments, enabled only when attached. |

Privileged handler code keeps the default memory map through `PRIVDEFENA`.
Unprivileged task code only sees the programmed user regions.

## Domains

An RK01 domain is a statically declared writable-authority boundary shared by
one or more tasks. Tasks remain independently scheduled entities. It is not a
process and has no user, file table or scheduler namespace.

The normal small-application shape is still RK0-like:

```c
RK_DECLARE_TASK(workerHandle, WorkerTask, workerStack, 256U)

kTaskInit(&workerHandle, WorkerTask, RK_NO_ARGS, "Worker",
          workerStack, 256U, WORKER_PRIO, RK_PREEMPT);
```

That places the task in the implicit `App` domain. Tasks in the same domain can
share domain RAM directly and should protect shared mutable state with ordinary
RK0-style services such as mutexes or semaphores.

Use explicit domains only when there is a real fault-containment boundary:

```c
RK_DECLARE_DOMAIN(controlDomain, controlRam, 4096U)
RK_DECLARE_DOMAIN_TASK(controlHandle, ControlTask)

kDomainInit(&controlDomain, controlRam, sizeof(controlRam), "Control");
kDomainTaskInit(&controlDomain, &controlHandle, ControlTask, RK_NO_ARGS,
                "Control", 256U, CONTROL_PRIO, RK_PREEMPT);
```

For service-style domains, prefer a source bundle:

| File | Role |
| --- | --- |
| `record_domain.h` | Public request/reply types and `RecordDomainBoot()` exports. |
| `record_domain_internal.h` | Private typed RAM layout and member task declarations. |
| `record_domain.c` | `RK_DECLARE_TYPED_DOMAIN()` storage, descriptor and BOOT construction. |
| `record_server.c` | One Record member task using the private RAM layout. |

The typed declaration creates an exact MPU-sized writable window while giving
the domain implementation a normal C struct view:

```c
typedef struct
{
    _Alignas(8) RK_STACK serverStack[256U];
    RecordState recordState;
    RK_TASK_HANDLE serverHandle;
} RECORD_DOMAIN_RAM;

RK_DECLARE_TYPED_DOMAIN(recordDomain, recordDomainRam,
                        RECORD_DOMAIN_RAM, 2048U)

RK_ERR RecordDomainBoot(RECORD_DOMAIN_EXPORTS *exportsPtr)
{
    RECORD_DOMAIN_RAM *const ramPtr = RK_DOMAIN_STATE(recordDomainRam);
    RK_ERR err;

    err = RK_DOMAIN_INIT_TYPED(&recordDomain, recordDomainRam, "Rec");
    if (err != RK_ERR_SUCCESS)
    {
        return err;
    }

    err = kTaskInitDomain(&ramPtr->serverHandle, RecordTask, ramPtr, "Record",
                          ramPtr->serverStack, 256U, RECORD_TASK_PRIO,
                          RK_PREEMPT, &recordDomain);
    if (err != RK_ERR_SUCCESS)
    {
        return err;
    }

    exportsPtr->serviceHandle = ramPtr->serverHandle;
    return RK_ERR_SUCCESS;
}
```

The linker generically collects `KEEP(*(.rk_domain_ram*))` into
`.rk_domain_ram`; it does not enumerate domain object files. BOOT validation
still enforces TASK_RAM placement, power-of-two size, natural alignment,
overlap checks, stack containment and topology finalisation before dispatch.
`DOMAIN_IMPL_SRCS` declares sources that should be audited for unexpected
writable globals; `make audit-domain-writable` runs the warning-only check.

Cross-domain payload transfer should normally use copied IPC.

## Syscalls And Handles

Public `k*` APIs are callable from privileged BOOT code and from unprivileged
task code. When the caller is unprivileged, wrappers enter SVC and the
dispatcher validates:

- syscall number and trap origin;
- handles and object type/generation state;
- user input buffer readability;
- user output buffer writability;
- callback/function pointer validity;
- packed argument structs before nested pointer use.

![User API call through SVC](docs/readme_svc_call.svg)

Runtime kernel objects are fixed-capacity pool entries. Application code keeps
opaque handles, not pointers to kernel control blocks:

```c
RK_DECLARE_LOCAL_SEMAPHORE(readySema)

kSemaphoreCreate(&readySema, "Ready", 0U, 1U);
kSemaphorePost(readySema);
kSemaphorePend(readySema, RK_WAIT_FOREVER);
kSemaphoreDestroy(&readySema);
```

## Synchronisation And IPC Contracts

Start with the RK0 interaction, then apply the RK01 memory rule.

| Interaction | RK01 rule |
| --- | --- |
| Shared memory | Direct load/store is allowed only where the MPU maps the same RAM into the participating tasks. Use mutexes for shared-memory ownership. |
| Asynchronous direct message | Transfers message ownership. By-reference messages require memory both sides can access; copied async messages can cross non-shared domain boundaries. Pool ceilings apply to this ownership contract. |
| Synchronous send/receive | Blocking copy rendezvous: the sender waits until the receiver copies the payload. There is no reply and no receiver priority substitution. |
| Synchronous call/reply | Extended rendezvous: the caller waits for a reply and the server runs at caller effective priority while the call is queued or active. Syscall validation lets copied payloads cross non-shared domain boundaries. |
| Cross-domain notification | Task events or copied messages. |
| Cross-domain payload | Message queues, mailboxes, synchronous send/receive, synchronous call/reply or task-addressed copy messages. |
| Latest value | MRM is domain-local because leases are pointers. Use copied payloads or a small `RK_SHARED_MEM` snapshot across domains. |

RK01 deliberately keeps both shared-state services and message-passing services.
It does not force every local interaction into an actor model.

## Fault Diagnostics

MemManage faults are used to contain unprivileged MPU violations. When fault
printing is enabled, RK01 prints the first cause directly, for example:

> `MPU TASK FAULT: data access violation task=Echo tid=4 pc=0x08001234 lr=0x08005678 addr=0x20010000 cfsr=0x00000082 mmfsr=0x82 frame=1`

![Contained unprivileged MPU fault](docs/readme_fault_diagnostics.svg)

Fatal privileged/kernel memory faults report `RK_FAULT_MEM_ACCESS`. Contained
task faults are normally marked for cleanup; enabling
`RK_CONF_MPU_TASK_FAULT_FAIL_FAST` turns that into a first-fault halt for
debugging.

## Public API Headers

Use the narrowest public header that fits the code:

| Header | Use |
| --- | --- |
| `kapi_app.h` | Ordinary App tasks, local objects, copied IPC, timers, sleep and logging. |
| `kapi_domain.h` | BOOT code that declares explicit domains, isolated tasks or shared memory. |
| `kconsole.h` | Privileged console UART service, foreground RX ownership and bounded console writes. |
| `kapi_diag.h` | Optional diagnostics such as SysMon object naming and trace snapshots. |
| `kapi_trusted.h` | Privileged service setup and low-level trusted construction. |
| `kapi.h` | Compatibility umbrella. |

## Current Limits

This public drop is useful for source review, experimentation and early board
testing, but several areas still need release hardening:

- generated configuration-specific memory maps;
- a maintained public board-regression suite;
- richer supervisor fault records;
- measured SVC, PRIMASK, SysTick and PendSV budgets on hardware;
- a final decision on which historical transition notes should become public
  documentation;
- broader target qualification.

For now, use RK0 when you need the mature flat kernel documentation and RK01
when you specifically want the MPU/SVC containment model.

## License

RK01 source files use the Apache-2.0 SPDX identifier. See `LICENSE`.

The `middleware/littlefs` subset is the upstream LittleFS code used by RKFS and
keeps its own licence file in `middleware/littlefs/LICENSE.md`.
