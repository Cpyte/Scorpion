# Scorpion — Bare-Metal Kernel for Microcontrollers

Scorpion is a cooperative-multitasking, single-address-space kernel for
microcontrollers with 32-bit RISC-V machine-mode cores — plus one Xtensa
port. It currently runs on the **Raspberry Pi RP2350** (RV32IMAC+Zb*
Hazard3 cores) and, in an experimental capacity, on the **ESP32-C3**
(RV32IMC) and **ESP32-S3** (Xtensa LX7, call0 ABI). It provides a
minimal system-call interface, a hierarchical heap allocator, a bitmap-based
physical page allocator, a FUSE-like filesystem on external QSPI
flash (RP2350) or mask-ROM spiflash (ESP32-C3/S3), an ELF/SEF loader, and a driver registration framework — all from scratch
with no external library dependencies.

Platform-specific code lives behind one small contract (`platform.h`); see
[docs/platforms.md](docs/platforms.md) for the support matrix and
[docs/porting.md](docs/porting.md) to bring up a new chip.

---

## Table of Contents

- [Scorpion — Bare-Metal Kernel for Microcontrollers](#scorpion--bare-metal-kernel-for-microcontrollers)
  - [Table of Contents](#table-of-contents)
  - [Architecture Overview](#architecture-overview)
  - [Supported Platforms](#supported-platforms)
  - [Build System \& Pico SDK Integration](#build-system--pico-sdk-integration)
    - [Configuration](#configuration)
    - [Build](#build)
    - [Outputs](#outputs)
  - [Memory Layout](#memory-layout)
  - [Boot Sequence](#boot-sequence)
  - [Kernel Subsystems](#kernel-subsystems)
    - [1. Heap Memory Allocator (`alloc.c`)](#1-heap-memory-allocator-allocc)
    - [2. Physical Page Allocator (`palloc.c`)](#2-physical-page-allocator-pallocc)
    - [3. Process \& Scheduler (`lifecycle.c`)](#3-process--scheduler-lifecyclec)
    - [4. Machine Timer (`lifecycle.c`)](#4-machine-timer-lifecyclec)
    - [5. Context Switching (`context.c`, `context.S`)](#5-context-switching-contextc-contexts)
    - [6. Trap \& Syscall Handler (`trap.c`, `trap.S`)](#6-trap--syscall-handler-trapc-traps)
    - [7. UART Console (`console.c`)](#7-uart-console-consolec)
    - [8. Driver Framework (`driver.c`)](#8-driver-framework-driverc)
    - [9. Flash Storage (`flash.c`)](#9-flash-storage-flashc)
    - [10. FUSE Filesystem (`fuse.c`)](#10-fuse-filesystem-fusec)
    - [11. Executable Loader (`loader.c`)](#11-executable-loader-loaderc)
    - [12. ABI (`abi/scorpion.h`)](#12-abi-abiscorpionh)
  - [RP2350 Platform Details](#rp2350-platform-details)
  - [Project Structure](#project-structure)
  - [License](#license)

---

## Architecture Overview

```

Blah Blah.
Just read these diagrams.

┌─────────────────────────────────────────────────────────────┐
│                   User Processes (EL0)                      │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐            │
│  │  Process A  │  │  Process B  │  │  Process C  │  ...    │
│  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘            │
│        │ ecall         │ ecall         │ ecall              │
├────────┼───────────────┼───────────────┼────────────────────┤
│        ▼               ▼               ▼                    │
│  ┌──────────────────────────────────────────────────┐       │
│  │        Trap Handler  (machine mode)               │       │
│  │  dispatching 15 syscalls + machine-timer IRQ      │       │
│  └──────────────────────┬───────────────────────────┘       │
│                         │                                    │
│  ┌──────────────────────┼───────────────────────────┐       │
│  │         ▼            ▼            ▼               │       │
│  │  Scheduler        FUSE FS      Driver Framework   │       │
│  │  (lifecycle.c)   (fuse.c)      (driver.c)         │       │
│  │         │            │            │               │       │
│  │         ▼            ▼            ▼               │       │
│  │  Context Switch    Flash FS     Stage Buffer Pool │       │
│  │  (context.S)     (flash.c)     (driver.c)         │       │
│  └──────────────────────────────────────────────────┘       │
│                                                              │
│  ┌──────────────────────────────────────────────────┐       │
│  │  Memory Subsystem                                │       │
│  │  Bump Alloc → Free List → Buddy Alloc + palloc   │       │
│  └──────────────────────────────────────────────────┘       │
│                                                              │
│  ┌──────────────────────────────────────────────────┐       │
│  │  RP2350 Hardware (RISC-V Hazard3 cores)          │       │
│  │  XIP Flash @ 0x10000000  ·  SRAM @ 0x20000000    │       │
│  │  UART (PL011) @ 0x40070000                       │       │
│  │  SIO timer (mtime/mtimecmp) @ 0xd0000000          │       │
│  └──────────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────────┘
```

> **Note:** The Raspberry Pi Pico 2 (RP2350 RISC-V) is the stable target.
> The ESP32-C3 (RISC-V) and ESP32-S3 (Xtensa LX7) are supported
> experimentally — boot, scheduler, IPC, syscalls and flash-backed FUSE
> work; hardware user protection does not (neither chip offers a
> RISC-V-style PMP, and the S3 port runs all processes in ring 0). See
> [docs/platforms.md](docs/platforms.md) for the full status matrix.

Scorpion is a **cooperative single-address-space** kernel :(:
- Kernel code runs in machine mode (M-mode); user processes run in user mode (U-mode).
- Processes (threads) share a single address space and yield cooperatively via
  the `ecall` instruction (syscall 0).
- A **machine timer** (RISC-V platform timer in the SIO block) fires periodic
  interrupts that invoke `yield()` on the running process — enabling
  preemptive time-slicing even in a cooperative design.
- The scheduler is priority-ordered round-robin; a higher-priority process
  runs before a lower-priority one, and the idle process (lowest priority)
  executes the architecture idle instruction (`wfi` / Xtensa `waiti`)
  when nothing else is ready.
- Executables are loaded through a pluggable format interface; built-in
  loaders include **SEF** (Scorpion's native format), ELF, and flat binary.
- **PMP** (Physical Memory Protection) restricts U-mode access to a dedicated
  user arena. Kernel memory, peripheral MMIO, and flash are inaccessible to user
  code, enforced via RISC-V PMP TOR entries at boot.
  There is no virtual memory. Very complicated, but logically consistent.

---

## Supported Platforms

| Platform | Board | ISA | Status |
|----------|-------|-----|--------|
| `rp2350` | Raspberry Pi Pico 2 | RV32IMAC(B) Hazard3 | stable (2 cores, PMP, flash/FUSE) |
| `esp32c3` | ESP32-C3 dev boards | RV32IMC | experimental (single core, ROM-spiflash FUSE, no PMP) |
| `esp32s3` | ESP32-S3 dev boards | Xtensa LX7 (call0) | experimental (single core used, ROM-spiflash FUSE, ring 0) |

Full details, caveats and bring-up checklists: [docs/platforms.md](docs/platforms.md).
Adding a new chip: [docs/porting.md](docs/porting.md).

Xtensa-family ESP32 / S2 / S3 would require an `arch/xtensa` CPU port
(new context-switch and trap assembly for the windowed ABI) and are not
planned in the near term.

---

## Build System & Pico SDK Integration

Scorpion uses the **Raspberry Pi Pico SDK v2.3.0** — vendored at
`pico-sdk-2.3.0/` — **exclusively for its cross-compiler toolchain
configuration**. The kernel provides its own startup code (`crt0.S`), linker
script (`kernel.ld`), and all runtime components, so no Pico SDK libraries are
linked.

The Pico SDK's toolchain setup (`pico_sdk_import.cmake` → `pico_sdk_init.cmake`
→ `pico_pre_load_platform.cmake` → `pico_pre_load_toolchain.cmake`) is used
early in the CMake configuration to:

1. Select the **RP2350 RISC-V platform** (`PICO_PLATFORM=rp2350-riscv`).
2. Locate the **RISC-V GCC cross-compiler** (tries `riscv32-unknown-elf-gcc`
   first, then `riscv32-corev-elf-gcc`).
3. Set the correct **architecture flags** for the Hazard3 cores:
   `-march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -mabi=ilp32 -mfloat-abi=softfp`.

The Pico SDK's `pico_sdk_init()` macro is **never called**, which means only
the toolchain configuration is imported, not the SDK's hardware abstraction
libraries, startup code, or linker scripts.

### Prerequisites

- **CMake** ≥ 3.13
- **RISC-V GCC toolchain** (`riscv32-unknown-elf-gcc`, `riscv64-elf-gcc`, or
  `riscv32-corev-elf-gcc` on `$PATH`, or pointed to via `PICO_TOOLCHAIN_PATH`)
- **ninja** or **make**

### Configuration

```bash
# RP2350 (default)
cmake -B build .

# ESP32-C3
cmake -B build-c3 -DPLATFORM=esp32c3 .

# ESP32-S3 (needs xtensa-esp32s3-elf-gcc, e.g. under ~/xtensa-tools/)
cmake -B build-s3 -DPLATFORM=esp32s3 .
```

For the rp2350 target, the `pico_sdk_import.cmake` script automatically
picks up the vendored SDK at `pico-sdk-2.3.0/`. If you've installed the
SDK elsewhere, set `PICO_SDK_PATH` before configuring:

```bash
PICO_SDK_PATH=/path/to/pico-sdk cmake -B build -DPLATFORM=rp2350 .
```

If no 32-bit RISC-V triple is installed, the build falls back to any
RV32-capable GCC (e.g. homebrew's `riscv64-elf-gcc`) with the Hazard3
flags applied manually.

### Build

```bash
cmake --build build
```

### Outputs (rp2350)

| File                  | Description                             |
|-----------------------|-----------------------------------------|
| `build/WEW_scorpion`   | ELF executable (kernel)                |
| `build/WEW_scorpion.bin` | Flat binary (kernel only)           |
| `build/Scorpion.uf2`   | UF2 image (boot2 + kernel + controller) |
| `build/scorpion.map`   | Linker map with symbol addresses       |
| `build/controller.sef` | Controller process in SEF format    |
| `build/test_user.sef` | Test user process in SEF format     |

### Outputs (esp32c3)

| File                        | Description                              |
|-----------------------------|------------------------------------------|
| `build-c3/WEW_scorpion`     | ELF executable (kernel, RAM-run layout)  |
| `build-c3/Scorpion_esp32c3.bin` | ESP image — flash at offset `0x0` via esptool |

### Outputs (esp32s3)

| File                        | Description                              |
|-----------------------------|------------------------------------------|
| `build-s3/WEW_scorpion`     | ELF executable (kernel, Xtensa call0, RAM-run) |
| `build-s3/Scorpion_esp32s3.bin` | ESP image (chip_id 9) — flash at offset `0x0` via esptool |

---

## Memory Layout

The kernel occupies the RP2350's internal SRAM. The linker script
(`arch/riscv32/kernel.ld`) maps SRAM at `0x20000000`:

```
0x20000000 ┌─────────────────────┐  ◄── SRAM base (520 KB total)
            │ .text + .rodata    │
            ├─────────────────────┤
            │ .data + .bss       │
            ├─────────────────────┤
            │ Heap                │  bump → free-list → buddy allocator
            ├─ - - - - - - - - - -│  (16 KB stack guard)
            │ Stack               │  grows downward from _stack_top
0x2007FFFF └─────────────────────┘  end of SRAM
```

| Region      | Address       | Size    | Contents                         |
|-------------|---------------|---------|----------------------------------|
| Code+Data   | `0x20000000`  | varies  | `.text`, `.rodata`, `.data`, `.bss` |
| Heap        | after BSS     | ~SRAM remainder - 16K | bump + free-list + buddy allocator |
| Stack       | top of SRAM   | ~16K    | kernel stack (grows down)        |

> **Note:** The RP2350A (Pico 2) has 520 KB of SRAM. The heap uses the
> available memory after the kernel image, minus 16 KB for the stack.
> The `HEAP_SIZE` constant (default 1 MB) sets the maximum expected heap;
> the actual runtime size is determined by the linker.

---

## Boot Sequence

```
Reset vector
    │
    ▼
  _start (crt0.S)
    │
    ├─ 1. Clear mstatus (disable interrupts)
    ├─ 2. Clear mie    (mask all interrupts)
    ├─ 3. Set SP = _stack_top
    ├─ 4. Copy .data from FLASH → RAM
    ├─ 5. Clear .bss (zero-initialize)
    ├─ 6. Call trap_init()    → install trap vector (mtvec)
    └─ 7. Call main()
              │
              ▼
           kernel_main()
               │
                ├─ alloc_init()        → init bump + buddy allocators
                ├─ user_arena_init()   → init user memory region
                ├─ pmp_init()          → configure PMP (U-mode protection)
                ├─ trap_init()         → set mtvec to trap_vector
               ├─ flash_init()        → locate bootrom flash routines
               ├─ fuse_init()         → mount / format FUSE
               ├─ loader_init()       → register SEF, ELF, binary format handlers
               ├─ stage_pool_init()   → pre-allocate I/O buffers
               ├─ timer_init()        → enable RISC-V platform timer (MTIE)
               ├─ process_create(init)→ create init process (PRIV_CONTROLLER)
               ├─ scheduler_init()    → create idle + scheduler context
               └─ scheduler_start()   → enter scheduler (never returns seq)
```

---

## Kernel Subsystems

### 1. Heap Memory Allocator (`alloc.c`)

A three-tier allocator backed by a linker-placed heap region:

| Strategy    | Size class         | Mechanism                                      |
|-------------|--------------------|------------------------------------------------|
| **Bump**    | any (first fit)    | Linear bump from `heap_offset`; never freed    |
| **Free-list** | < 4 KB           | Singly-linked list of freed blocks, coalesced  |
| **Buddy**   | ≥ 4 KB (page-aligned) | Power-of-two block splitting/coalescing   |

- `alloc_(n)` → returns `n` bytes (bump for first allocation, then free-list
  for small, buddy for large).
- `free_(p)` → returns memory to the free-list or buddy allocator
  (auto-detected by page alignment and metadata).
- Thread-safe via spinlocks.
- Buddy metadata (`BuddyPage`) stored in a static array sized for the heap.

### 2. Physical Page Allocator (`palloc.c`)

A bitmap-based page allocator for managing physical memory (separate from the
heap allocator).

- `palloc_init(base, size, bitmap, bitmap_size)` — initialise the allocator
  for a physical memory region.
- `palloc()` / `palloc_contiguous(n)` — allocate 1 or `n` contiguous pages.
- `pfree()` / `pfree_contiguous()` — free pages back.
- Tracks all pages with a bit array; marks all pages as used initially, then
  `palloc_free_region()` marks free pages.
- Thread-safe via spinlock.

### 3. Process & Scheduler (`lifecycle.c`)

**Process states:** `UNUSED → READY → RUNNING → (BLOCKED | TERMINATED)`

- **`add_process(proc, priority)`** — inserts a process into a priority-ordered
  doubly-linked list (lower numeric = higher priority, `UINT16_MAX` = idle).
- **`scheduler_init()`** — sets up the scheduler stack and context, creates the
  idle process (executes the idle instruction + `yield`).
- **`scheduler_start()`** — context-switches from `main()` into the scheduler.
- **`scheduler_entry()`** — the scheduler loop: reap terminated processes,
  then run the next ready process.
- **`runprocess()`** — scans the queue for a `READY` process and
  context-switches to it. Falls back to the idle process if none are ready.
- **`yield()`** — sets current process to `READY`, marks it (via `exclude_list`)
  to prevent immediate rescheduling, and switches back to the scheduler.
- **`block_process()`** — sets current process to `BLOCKED` and switches to
  scheduler.
- **`wake_process(p)`** — transitions a blocked process back to `READY`.
- **`killprocess()`** — frees all resources for `TERMINATED` processes.
- **`process_exit()`** — marks current process `TERMINATED` and switches to
  scheduler.

Up to **4 cores** (`MAX_CORES`) are supported, each with its own scheduler
context and current-process pointer.

On SMP builds every queue operation (`add_process`, `runprocess`,
`killprocess`, `wake_sleeping_processes`, `process_by_pid`,
`evict_lowest_priority`) runs under a global `queue_lock` with interrupts
masked (`irq_save`/`irq_restore`): masking prevents a same-core timer-ISR
`yield()` from re-entering a queue critical section and self-deadlocking, and
the lock keeps remote cores out. The lock is **never held across
`context_switch`** — it is released before switching, re-acquired on resume.
Lock order is strict: `queue_lock` → allocator locks → `console_lock`.

### 4. Machine Timer (`lifecycle.c`)

The RP2350 provides a RISC-V platform timer in the SIO register block
(`0xd0000000`). It consists of a shared 64-bit counter (`mtime`) and a
per-core 64-bit comparator (`mtimecmp`). The timer interrupt is routed to
machine-level interrupt 7.

- **`timer_init()`** — zeroes mtime, sets mtimecmp = `TIMER_INTERVAL`
  (1,000,000 cycles), enables MTIE in `mie` CSR, and starts the counter.
- **`timer_irq()`** — called from the trap handler on mcause=0x80000007.
  Advances mtimecmp by `TIMER_INTERVAL` and calls `yield()` to preempt the
  running process.
- The timer is initialised in `kernel_main()` before the scheduler starts.
- The interval is a tunable constant; at 150 MHz it yields roughly every
  6.7 ms.

### 5. Context Switching (`context.c`, `context.S`)

- `RiscVContext` holds callee-saved registers + `mstatus`:
  `{pc, ra, sp, gp, tp, s0-s11, mstatus}`.
- `context_switch(old, next)` — assembly function in `context.S`:
  - Saves `ra, sp, gp, tp, s0-s11` to `*old` (if non-NULL).
  - Saves/restores `mstatus` CSR (critical for interrupt state across switch).
  - Restores `ra, sp, gp, tp, s0-s11` from `*next`.
  - Jumps to the saved PC in `*next`.
- Process stacks are 4 KB, allocated from the heap and 16-byte aligned.
- The trampoline (`process_trampoline()`) calls the process entry function and
  invokes `process_exit()` on return.
- Kernel contexts initialise `mstatus = 0x1808` (MIE=1, MPP=3).
  User contexts initialise `mstatus = 0x0088` (MIE=1, MPP=0).

### 6. Trap & Syscall Handler (`trap.c`, `trap.S`)

Machine-mode trap handling:

- **`trap_vector`** (assembly in `trap.S`):
  - Saves all 32 integer registers plus `mcause`/`mepc`/`mstatus` into a
    `RiscVTrapFrame` (140 bytes) on the stack.
  - Calls `trap_handler(frame)`.
  - Restores registers and executes `mret`.
- **`trap_handler`** (C in `trap.c`, shared across architectures):
  - Timer interrupts are recognized via `platform_timer_is_irq()`
    (each platform publishes its own synthesized mcause): re-arms the
    comparator and calls `yield()` to preempt the running process.
  - On `ecall`/`syscall`: dispatches by syscall number (RV32: `a7`;
    Xtensa: `a2`) through the arch-neutral `TRAP_*` macros.
  - On any other exception: kills the faulting user process or calls
    `panic()`.

**System calls (15 total):**

| #  | Name      | Arguments               | Description                              |
|----|-----------|-------------------------|------------------------------------------|
| 0  | YIELD     | —                       | Voluntarily yield the CPU               |
| 1  | EXIT      | —                       | Terminate the current process           |
| 2  | BLOCK     | —                       | Block the current process               |
| 3  | WAKE      | `a0=pid`              | Wake a blocked process by PID          |
| 4  | SLEEP     | `a0=ticks`             | Sleep for `ticks` scheduler iterations  |
| 5  | SEND      | `a0=pid, a1=type, a2=data, a3=len` | Send IPC message to process `pid` |
| 6  | RECV      | `a0=type, a1=buf, a2=len, a3=&sender` | Receive IPC message              |
| 7  | OPEN      | `a0=path, a1=mode`     | Open/create a file (returns fd)         |
| 8  | READ      | `a0=fd, a1=buf, a2=len` | Read from a file                       |
| 9  | WRITE     | `a0=fd, a1=buf, a2=len` | Write to a file                        |
| 10 | CLOSE     | `a0=fd`                | Close a file descriptor                 |
| 11 | PUTC      | `a0=str, a1=len`       | Write string to UART console            |
| 12 | SPAWN     | `a0=sef_data, a1=size, a2=priority` | Spawn a new process from SEF data |
| 13 | TERMINATE | `a0=pid`              | Terminate a process by PID             |
| 14 | LOADLIB   | `a0=sef_data, a1=size` | Dynamically link a SEF library into   the calling process              |

### 7. UART Console (`console.c`)

- MMIO PL011 UART at base address `0x40070000` (UART0).
- **Initialisation**: `console_init()` configures GPIO0/1 for UART function
  (IO_BANK0 function select 2), sets 115200 baud, 8n1 framing, and enables the
  UART.
- `console_putchar(c)`, `console_write(s, len)`, `console_puts(s)`.
- **Line buffering** — kernel diagnostics through `console_write` (the PUTC
  syscall path) go straight to the UART; `console_puts`/`log_*` output is
  buffered in a 128-byte line buffer (`CONSOLE_BUF_MAX`) and flushed on
  newline or when full via `console_flush()`. A `console_lock` spinlock plus
  masked interrupts (`irq_save`/`irq_restore`) protect every output path.
- Formatted logging: `log_info`, `log_warn`, `log_error`, `panic` with
  `%s`, `%d`, `%u`, `%x`, `%p` format specifiers.

### 8. Driver Framework (`driver.c`)

A generic driver registration and I/O framework:

- `Driver` struct with callbacks: `init`, `open`, `close`, `write`, `read`, `ioctl`.
- `register_driver()` / `find_driver()` — linked-list registration.
- **Stage Buffer Pool** — pre-allocates a pool of `StageBuffer` structures with
  data buffers for asynchronous I/O:
  - `stage_pool_init(count, buf_size)` — initialise the pool.
  - `stage_alloc()` / `stage_free()` — acquire/release buffers.
  - `stage_submit(drv, buf)` — submit a buffer to a driver's `write` callback.

### 9. Flash Storage (`flash.c`)

- Platform backends behind a common interface: RP2350 uses XIP reads
  (`0x10000000`) plus bootrom function table for erase/program; ESP32-C3
  and ESP32-S3 use the legacy SPI-flash driver in mask ROM.
- 256 blocks × 256 bytes = 64 KB total, mapped at device offset
  `FLASH_STORAGE_OFFSET` (rp2350: `0x20000`, esp32c3/s3: `0x10000`) so FUSE
  can never clobber the boot image or embedded payloads.
- `flash_read()`, `flash_write()`, `flash_erase()` — block-oriented operations.
- **Write-back sector cache** — `flash_write()` patches a 4 KB sector image in
  RAM (`FLASH_CACHE_SLOTS` such images; NOR flash requires erase-to-ones, so a
  raw write cannot be done in place). Dirty sectors are flushed to flash with
  erase+program by `flash_sync()` (called at FUSE commit points) or when a slot
  must be evicted. `flash_read()` and `flash_erase()` consult the cache first,
  so within-session reads observe the dirty image. Erase uses 4 KB sector
  granularity (bootrom `flash_range_erase` with `FLASH_SECTOR_SIZE`).
- Cache access is serialized by a `flash_cache_lock` spinlock with interrupts
  masked during RAM-side patching/eviction.
- RP2350 bootrom functions located by code lookup (`ROM_CODE('R', 'E')`, etc.)
  through the RP2350's `rom_table_lookup`.

### 10. FUSE Filesystem (`fuse.c`)

A simple flat filesystem layered on the platform flash backend:

- **Superblock** (block 0): magic number (`0x53434653`), entry count, data start.
- **Directory entries**: up to 16 files, 24-character names, size, first block, mode.
- **File descriptors**: up to 16 open files simultaneously.
- **Operations**: `fuse_open`, `fuse_close`, `fuse_read`, `fuse_write`, `fuse_list`.
- **RAM directory cache** — entry blocks are cached in RAM on open and written
  back on `fuse_close` (close = commit). Writes within a session hit the flash
  write-back cache and are flushed to flash at commit. Power loss before close
  loses uncommitted writes; `fuse_init` re-reads the directory from flash.
- Auto-formats the flash if no valid superblock is found.

### 11. Executable Loader (`loader.c`)

An extensible, pluggable executable loader:

- `loader_register_format(format)` — register a loader for a custom executable
  format (identified by magic bytes or name string).
- `loader_load(data, size, proc)` — dispatch to the appropriate format handler
  based on the input data.
- Built-in format handlers:
  - **SEF** — Scorpion's native format (magic `0x00464553`). Segments are
    described by a header array; supports TEXT, DATA, and BSS segments. The
    `SEF_FLAG_PRIV_CONTROLLER` flag sets the process privilege level.
  - **ELF** — validates a 32-bit RISC-V ELF header; loads all `PT_LOAD`
    segments into a single contiguous allocation; distinguishes text vs. data
    segments by ELF flags (`PF_X`, `PF_W`); sets `PC` = ELF entry point.
  - **Raw binary** — loads a flat binary with a specified entry address.
    Registered last; rejects data matching ELF or SEF magic.
- `process_create(entry, arg)` — allocates and initialises a `Process` struct
  for a given entry function.

**SEF format structure:**

```
┌─────────────────────────┐
│  uint32 magic 0x00464553 │
│  uint32 entry            │
│  uint16 num_segments     │
│  uint16 flags            │
├─────────────────────────┤
│  SefSegment[0]         │
│    type (0=TEXT,1=DATA,2=BSS)│
│    vaddr (offset from base)  │
│    size                     │
│    offset (in file)         │
│  SefSegment[1]         │
│  ...                     │
├─────────────────────────┤
│  Raw segment data        │
└─────────────────────────┘
```

The `mksef.py` tool converts an ELF with `.text`, `.rodata`, `.data`, and
`.bss` sections into the SEF format. The `packrom.py` tool embeds the
controller SEF binary at a fixed flash offset alongside the kernel during the
UF2 build step.

### 12. ABI (`abi/scorpion.h`)

Header for user-space programs compiled against the Scorpion kernel:

- Defines syscall numbers as macros.
- Provides inline-assembly wrappers for each syscall using RISC-V register
  conventions (`a7` = syscall number, arguments in `a0`–`a5`). The S3
  port has no SEF dynamic linking yet, so RV32 payloads are not built
  for it; kernel-side dispatch is architecture-neutral via the `TRAP_*`
  macros in `scorpion.h`.

---

## RP2350 Platform Details

Scorpion's stable target is the **Raspberry Pi RP2350** in **RISC-V
mode**, where the two CPU cores implement the **Hazard3** RISC-V ISA
(RV32IMAC). Other platforms (ESP32-C3 and ESP32-S3 today) are documented in
[docs/platforms.md](docs/platforms.md).

Key RP2350 features relevant to Scorpion:

| Feature             | Details                                  |
|---------------------|------------------------------------------|
| **Cores**           | 2 × Hazard3 RISC-V (RV32IMAC + B‑ext)  |
| **SRAM**            | 512 KB (SRAM0–SRAM4, address `0x20000000`) |
| **XIP Flash**       | External QSPI flash via `0x10000000`     |
| **UART**            | PL011 at `0x40070000` (UART0)           |
| **GPIO**            | IO_BANK0 at `0x40028000` (function select per pin) |
| **Machine Timer**   | RISC-V platform timer in SIO block at `0xd0000000`, 64-bit `mtime` (shared), `mtimecmp` (per-core). Timer IRQ is machine-level interrupt 7. |
| **SIO**             | `0xd0000000`: inter-core FIFO, soft IRQ, mtime/mtimecmp, TMDS |
| **Boot**            | ROM loads user binary from flash into SRAM or XIP |
| **Arch flags**      | `-march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -mabi=ilp32` |

The Pico SDK's RISC-V toolchain file (`cmake/preload/toolchains/pico_riscv_gcc.cmake`)
selects the `hazard3` system processor and uses the `B-extension` flags
(`zba`, `zbb`, `zbs`, `zbkb`) that the Hazard3 cores implement for
bit-manipulation instructions okay?

---

## Project Structure (complicated)

```
├── CMakeLists.txt              # Build: PLATFORM=rp2350 | esp32c3
├── README.md                   # This file
├── platform.h                  # Hardware abstraction contract (see docs/porting.md)
├── scorpion.h                  # Master kernel header (types, IPC, scheduler API)
├── sef.h                       # SEF executable format struct definitions + PMP constants
├── main.c                      # Kernel entry: boots subsystems, spawns init, starts scheduler
├── alloc.c / alloc.h           # Heap allocator (bump + free-list + buddy)
├── palloc.c                    # Physical page allocator (bitmap-based)
├── lifecycle.c                 # Process scheduler, yield, block, wake, kill
├── context.c                   # Context init, current_core_id(), trampoline
├── trap.c                      # Syscall dispatcher + fault handling
├── console.c / console.h       # Formatting/logging (hardware via platform layer)
├── driver.c / driver.h         # Driver registration framework + stage buffers
├── flash.c                     # Generic block layer over platform flash primitives
├── fuse.c / fuse.h             # FUSE filesystem on flash
├── loader.c / loader.h         # Pluggable executable loader (SEF, ELF, binary)
├── platform/
│   ├── rp2350/                 # Stable target: uart, SIO timer, SMP handshake,
│   │                           # bootrom flash, PMP, XIP payload reader
│   ├── esp32c3/                # Experimental: ROM UART0, SYSTIMER+INTC timer,
│   │                           # ROM spiflash flash, single-core stubs,
│   │                           # atomic compat shim
│   └── esp32s3/                # Experimental: ROM UART0, SYSTIMER + interrupt
│                               # matrix timer, ROM spiflash, single-core stub
├── arch/
│   ├── riscv32/
│   │   ├── crt0.S              # C runtime: reset vector, data/BSS init
│   │   ├── context.S           # Context switch assembly (saves mstatus)
│   │   ├── trap.S              # Trap vector, full frame save/restore
│   │   ├── kernel.ld           # rp2350 linker script (SRAM layout)
│   │   └── kernel_esp32c3.ld   # esp32c3 linker script (IRAM/DRAM, RAM-run)
│   └── xtensa/
│       ├── crt0.S              # Reset entry: VECBASE, BSS init (call0 ABI)
│       ├── context.S           # Context switch ({pc, sp, a12–a15})
│       ├── vectors.S           # VECBASE table + trap entry/exit (rfe)
│       ├── trap.c              # trap_init() — re-asserts VECBASE
│       ├── runtime_helpers.c   # div/mod/clz/shifts (no call0 libgcc)
│       └── kernel_esp32s3.ld   # esp32s3 linker script (IRAM/DRAM, RAM-run)
├── abi/
│   └── scorpion.h              # User-space syscall inline wrappers
├── docs/
│   ├── exec-format.md          # SEF/ELF/BIN loader formats
│   ├── dynamic-linking.md      # SEF v2 imports/exports
│   ├── platforms.md            # Support matrix + per-platform details
│   └── porting.md              # How to add a new platform
├── tools/
│   ├── packrom.py              # UF2 packer: boot2 + kernel + controller SEF
│   ├── elf2sef.py              # ELF → SEF converter
│   └── mkimage_esp32c3.py      # Kernel ELF → flashable ESP32-C3 image
├── user/
│   ├── controller.S            # Controller process (PRIV_CONTROLLER)
│   └── test_user.S             # Test user process (PRIV_USER)
└── pico-sdk-2.3.0/             # Vendored Raspberry Pi Pico SDK v2.3.0
    └── ...                     # (used for toolchain configuration only)
```

---

## License

This project is provided for educational and development purposes. See
individual source files for license terms...... Actually just fork this.
I'm happy if anyone even see this.

## Random Trash
This section serves no purpose.
If you are reading this, you have found the useless bytes (yes, this is intentional nonsense).

--------------------------------------------------------

∆∆∆´∂πø˜πµπçµø™µ–™πµåß¬…çµœ´µœ≤£“ƒ≤œπ¬œπ“œ´“π‘√««™√‘∑≤´√
ˆø´˜øˆƒ˜œπ˜∑πøœ˜πøµøπœ∑µƒøπœµƒøπµœøπƒµπøµç¬çœ–πµ™–ºµºª™£