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

## Major Changes

RK01 is not a "better RK0" or an embedded RTOS trying to be a GPOS. It is still
one statically linked firmware image for a microcontroller. The kernel, startup
code, board port, privileged service tasks and build are trusted. Ordinary
application tasks are treated as possibly defective after dispatch. If one
fails, the fault is confined and then handled as well as the application can.

Immediate differences from RK0:

| Aspect | RK0 | RK01 |
| --- | --- | --- |
| Kernel boundary | Kernel and application code share privileged address space. | Ordinary tasks run with no privilege at all. Kernel services are 'supervisor calls' -- software interrupts. |
| Memory protection | Cooperative discipline. | Cortex-M MPU regions protect kernel RAM, domain RAM and shared apertures. |
| Application grouping | Tasks can directly share C globals. | Tasks share memory only inside their domain, global shared RAM or explicit shared memory. |
| Kernel objects | Raw/static objects are natural; pool-backed creation is optional. | Runtime objects are fixed-capacity kernel pool entries _encoded_ by opaque handles. That is, you cannot dereference a handle because it is not an address. |
| IPC rule | Pointer transfer is fine when firmware agrees. | By-reference direct messages are same-domain only; cross-domain data should be copied. |
| Bad syscall pointers | A bad pointer can become a privileged fault if unchecked. | SVC validates user read/write/function ranges before privileged code dereferences them. |
| Fault handling | Serious task faults usually become system faults. | Unprivileged MemManage faults can be contained, marked `FAULT_PENDING` and cleaned by PostProc. |

>💡 The Real-Time Model that makes the interaction-based service design meaningful has not changed.

- `RK0` is flat trusted real-time firmware;
- `RK01` is RK0-style real-time with user space/kernel space.

## Architecture Sketch

![RK01 containment sketch](docs/readme_architecture_sketch.svg)

## Containment Scope
Its protection boundary is practical and local:

- ordinary tasks run unprivileged and enter kernel services through Supervisor Calls;
- kernel RAM, object pools, registries and privileged stacks remain
  privileged-only;
- each domain provides a statically declared writable-authority boundary shared
  by one or more tasks;
- cross-domain data moves through copied IPC, global shared RAM or explicitly
  attached shared memory;
- unprivileged MemManage faults can be recorded, contained and cleaned up by
  PostProc.

The trusted base remains the kernel, startup code, board port, privileged
service tasks, build, DMA setup, debug access and physical device access.

RK01 focuses on preventing defective unprivileged task code from corrupting kernel
RAM and another domain's writable state or privileged service state.

## Delivered Supported Targets

RK0's sweet spot is Cortex-M0, M3 and M4F when FPU work is intense. M7 is
probably too much.

RK01's sweet spot is M4F and M7 chips with MPU. ARMv8M chips are supported but
would be underused.

This repo delivers a build environment to run on Nucleo STM32F401RE M4F, and a
QEMU environment for MPS2 Cortex-M33. RK01 does not use the _Trusted
Environment_.

Although Cortex-M0+ chips have MPUs, RK01 does not support ARMv6M.


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


## Build Requirements

- Unlike RK0, which stays aligned with C99, RK01 requires at least C11. C11
  introduces memory-alignment features that would otherwise be cumbersome or
  infeasible in C99.

Required for firmware builds:

- GNU Make (C11/GNU11)
- `arm-none-eabi-gcc`
- `arm-none-eabi-objcopy`
- `arm-none-eabi-size`

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

Build the STM32F401RE preemption profiles with `RK_CONF_SYSTICK_DIV=1000`:

```sh
make -j4 ARCH=armv7m PLATFORM=stm32f401re APP_EXAMPLE=05-profile-preempt EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000"
make -j4 ARCH=armv7m PLATFORM=stm32f401re APP_EXAMPLE=05-profile-preempt EXTRA_DEFS="-DNDEBUG -DRK_CONF_SYSTICK_DIV=1000 -DPROFILE_PREEMPT_CLASS=PROFILE_PREEMPT_CLASS_PER_TASK_DOMAIN"
```

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
task's effective MPU configuration combines its domain, private stack, global
shared RAM and any explicit shared-memory segments attached to that domain.

![STM32F401RE memory domains](docs/readme_memory_model.svg)

| Region | Access |
| --- | --- |
| Flash | User-readable and executable. On STM32F401RE this is `0x08000000..0x08040000`. |
| FS_FLASH | STM32F401RE reserved flash at `0x08040000..0x08080000`, used by RKFS. Not user executable. |
| Domain RAM | Writable only by tasks whose current MPU view maps that domain; ordinary task stacks live outside this window. |
| Task stack RAM | Private stack storage mapped only for the active task that owns it. |
| Global shared RAM | Small firmware-wide aperture mapped into ordinary tasks. |
| Explicit shared memory | Boot-created TASK_RAM segment attached only to selected domains. |
| Kernel RAM | Privileged only. Contains TCBs, object pools, registries and privileged stacks. |

Typical MPU slot intent:

| MPU slot | Meaning |
| --- | --- |
| Region 0 | User-readable executable Flash, installed once. |
| Region 1 | Active immutable domain RAM authority window. |
| Region 2 | Active task's private stack. |
| Region 3 | Global shared RAM. |
| Regions 4..7 | Explicit shared-memory segments, enabled only when attached. |

Privileged handler code keeps the default memory map through `PRIVDEFENA`.
Unprivileged task code only sees the programmed user regions.
On every dispatch to a different task, RK01 replaces the task-private stack MPU
region. When dispatch also crosses a domain boundary, it replaces the domain RAM
and shared-authority regions. Scheduling semantics remain unchanged.

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

It is advisable to use more than a single domain only when there is a real fault-containment boundary:

```c
RK_DECLARE_DOMAIN(controlDomain, controlRam, 4096U)
RK_DECLARE_DOMAIN_TASK(controlHandle, ControlTask)
RK_DECLARE_DOMAIN_TASK_STACK(controlStack, 256U)

kDomainInit(&controlDomain, controlRam, sizeof(controlRam), "Control");
kTaskInitDomain(&controlHandle, ControlTask, RK_NO_ARGS, "Control",
                controlStack, 256U, CONTROL_PRIO, RK_PREEMPT,
                &controlDomain);
```

And for each domain create its .c and .h, like in the provided example:


| File | Role |
| --- | --- |
| `record_domain.h` | Public request/reply types and `RecordDomainBoot()` exports. |
| `record_domain_internal.h` | Private typed RAM layout and member task declarations. |
| `record_domain.c` | `RK_DECLARE_TYPED_DOMAIN()` storage, descriptor and BOOT construction. |
| `record_server.c` | One Record member task using the private RAM layout. |

The typed declaration creates an exact MPU-sized writable window while giving
the domain implementation a normal C struct view:

```c
RK_DECLARE_DOMAIN_RAM(RECORD_DOMAIN_RAM,
    RK_DOMAIN_RAM_MEMBER(RecordState, recordState)
    RK_DOMAIN_RAM_TASK_HANDLE(serverHandle)
)

RK_DECLARE_TYPED_DOMAIN(recordDomain, recordDomainRam,
                        RECORD_DOMAIN_RAM, 1024U)
RK_DECLARE_DOMAIN_TASK_STACK(recordServerStack, 256U)

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
                          recordServerStack, 256U, RECORD_TASK_PRIO,
                          RK_PREEMPT, &recordDomain);
    if (err != RK_ERR_SUCCESS)
    {
        return err;
    }

    exportsPtr->serviceHandle = ramPtr->serverHandle;
    return RK_ERR_SUCCESS;
}
```

- The linker generically collects `KEEP(*(.rk_domain_ram*))` into
`.rk_domain_ram`; it does not enumerate domain object files.

- BOOT validation still enforces TASK_RAM placement, **power-of-two size, natural alignment,
overlap checks, stack placement and separation, and topology finalisation before dispatch.**

`DOMAIN_IMPL_SRCS` declares files should not define unexpected writable globals in sections like:
```
.data
.bss
.sdata
.sbss
COMMON
```
It catches this kind of mistake:
```c
/* linker cant know where to place this: */
static ULONG counter; // bang
```

```c
/* you must speak up: */
RK_DECLARE_DOMAIN_RAM(RECORD_DOMAIN_RAM,
    RK_DOMAIN_RAM_MEMBER(ULONG, counter)
    RK_DOMAIN_RAM_MEMBER(RecordState, recordState)
    RK_DOMAIN_RAM_TASK_HANDLE(serverHandle)
)
```

## Syscalls And Object Life Cycle


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

Kernel object representation and usage are probably the most radical changes
compared with RK0. RK0 tried to keep object creation and access as direct as
possible. In RK01, objects are fully opaque: a handle is a number the kernel
resolves. Every kernel object is an `RK_HANDLE` subclass; object slots are
allocated and deallocated from pools whose maximum size is declared at compile
time.

```c
/* ready sema is visible for every task in this domain */
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
| Shared memory | Direct load/store is allowed only where the MPU maps the same RAM into the participating tasks. Coordinate shared-memory access between domains. |
| Asynchronous direct message | Transfers message ownership. By-reference direct messages are same-domain only; copied async messages can cross non-shared domain boundaries. Priority ceilings apply to the by-reference ownership contract. |
| Synchronous send/receive | Blocking copy rendezvous: the sender waits until the receiver copies the payload. There is no reply and no receiver priority substitution. |
| Synchronous call/reply | Extended rendezvous: the caller waits for a reply and the server runs at caller effective priority while the call is queued or active. Syscall validation lets copied payloads cross non-shared domain boundaries. |
| Cross-domain notification | Task events or indirect messages. |
| Cross-domain payload through indirect messages | Message queues and mailboxes need global scope so tasks in different domains can resolve the same handle. |
| Named task-backed message passing | Synchronous send/receive, synchronous call/reply and task-addressed copy messages copy payloads through the syscall boundary. |
| Latest value | MRM is domain-local because leases are pointers. Use copied payloads or a small `RK_SHARED_MEM` snapshot across domains. |

RK01 deliberately keeps both shared-state services and message-passing services. _It does not force every local interaction into an actor model, you know better._

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


## License

RK01 source files use the Apache-2.0 SPDX identifier. See `LICENSE`.

The `middleware/littlefs` subset is the upstream LittleFS code used by RKFS and
keeps its own licence file in `middleware/littlefs/LICENSE.md`.
