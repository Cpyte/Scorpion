# Scorpion — Bare-Metal Kernel for Raspberry Pi RP2350 (RISC-V)

Scorpion is a cooperative-multitasking, single-address-space kernel for the
Raspberry Pi RP2350 microcontroller running in **RISC-V 32-bit mode**
(RV32IMAC + bit‑manipulation extensions, Hazard3 cores). It provides a
minimal system-call interface, a hierarchical heap allocator, a bitmap-based
physical page allocator, a FUSE-like filesystem on an emulated flash device,
an ELF loader, and a driver registration framework — all from scratch with no
external library dependencies.

---

## Table of Contents

- [Scorpion — Bare-Metal Kernel for Raspberry Pi RP2350 (RISC-V)](#scorpion--bare-metal-kernel-for-raspberry-pi-rp2350-riscv)
  - [Table of Contents](#table-of-contents)
  - [Architecture Overview](#architecture-overview)
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
    - [4. Context Switching (`context.c`, `context.S`)](#4-context-switching-contextc-contexts)
    - [5. Trap \& Syscall Handler (`trap.c`, `trap.S`)](#5-trap--syscall-handler-trapc-traps)
    - [6. UART Console (`console.c`)](#6-uart-console-consolec)
    - [7. Driver Framework (`driver.c`)](#7-driver-framework-driverc)
    - [8. Emulated Flash (`flash.c`)](#8-emulated-flash-flashc)
    - [9. FUSE Filesystem (`fuse.c`)](#9-fuse-filesystem-fusec)
    - [10. ELF \& Binary Loader (`loader.c`)](#10-elf--binary-loader-loaderc)
    - [11. ABI (`abi/scorpion.h`)](#11-abi-abiscorpionh)
  - [RP2350 Platform Details](#rp2350-platform-details)
  - [Project Structure](#project-structure)
  - [License](#license)

---

## Architecture Overview

```
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
│  │  dispatching 12 syscalls (yield, sleep, read...)  │       │
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
│  │  UART @ 0x10000000  ·  SRAM @ 0x20000000          │       │
│  └──────────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────────┘
```

> **Note:** The current build does **not** yet include support for external
> controllers (gamepad, display drivers, etc.). Controller support is planned
> for a future release.

Scorpion is a **cooperative single-address-space** kernel:
- All code runs in machine mode (M-mode); there is no separate supervisor mode.
- Processes (threads) share a single address space and yield cooperatively via
  the `ecall` instruction (syscall 0).
- The scheduler is priority-ordered round-robin; a higher-priority process
  runs before a lower-priority one, and the idle process (lowest priority)
  executes `wfi` when nothing else is ready.
- The ELF loader maps `PT_LOAD` segments into the flat address space and jumps
  to the entry point. There is no virtual memory.

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
cmake -B build -G Ninja .
```

The `pico_sdk_import.cmake` script automatically picks up the vendored SDK
at `pico-sdk-2.3.0/`. If you've installed the SDK elsewhere, set
`PICO_SDK_PATH` before configuring:

```bash
PICO_SDK_PATH=/path/to/pico-sdk cmake -B build -G Ninja .
```

### Build

```bash
cmake --build build
```

### Outputs

| File                | Description                             |
|---------------------|-----------------------------------------|
| `build/WEW_scorpion` | ELF executable (kernel)                |
| `build/WEW_scorpion.bin` | Flat binary for loading into SRAM |
| `build/scorpion.map` | Linker map with symbol addresses       |

---

## Memory Layout

The kernel occupies the RP2350's internal SRAM. The linker script
(`arch/riscv32/kernel.ld`) defines two regions:

```
0x20000000 ┌─────────────────────┐  ◄── SRAM0 base (512 KB total)
            │ .text + .rodata    │
            ├─────────────────────┤
            │ .data + .bss       │
            ├─────────────────────┤
            │ Heap (≤ 384 KB)    │  bump → free-list → buddy allocator
            ├─────────────────────┤
            │ 16 KB guard         │  stack overflow detection
            ├─────────────────────┤
            │ Stack               │  grows downward from _stack_top
0x2007FFFF └─────────────────────┘  end of SRAM
```

| Region      | Address       | Size    | Contents                         |
|-------------|---------------|---------|----------------------------------|
| Code+Data   | `0x20000000`  | ≤512K   | `.text`, `.rodata`, `.data`, `.bss`, heap |
| Stack       | top of SRAM   | ~16K    | kernel stack (grows down)        |

> **Note:** The RP2350 has 520 KB of SRAM total (SRAM0–SRAM4 at `0x20000000`).
> The heap array is statically sized at 384 KB (`HEAP_SIZE=393216`); the
> remaining SRAM after code, data, and BSS provides the effective heap.

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
              ├─ alloc_init()      → init bump + buddy allocators
              ├─ trap_init()       → set mtvec to trap_vector
              ├─ flash_init()      → zero emulated flash
              ├─ fuse_init()       → mount / format FUSE
              ├─ stage_pool_init() → pre-allocate I/O buffers
              ├─ process_create()  → create test process
              ├─ scheduler_init()  → create idle process + scheduler context
              └─ scheduler_start() → enter scheduler (never returns)
```

---

## Kernel Subsystems

### 1. Heap Memory Allocator (`alloc.c`)

A three-tier allocator built on a static `heap[]` array (384 KB):

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
  idle process (executes `wfi` + `yield`).
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

### 4. Context Switching (`context.c`, `context.S`)

- `RiscVContext` holds callee-saved registers: `{pc, ra, sp, gp, tp, s0-s11}`.
- `context_switch(old, next)` — assembly function in `context.S`:
  - Saves `ra, sp, gp, tp, s0-s11` to `*old` (if non-NULL).
  - Restores `ra, sp, gp, tp, s0-s11` from `*next`.
  - Jumps to the saved PC in `*next`.
- Process stacks are 4 KB, allocated from the heap and 16-byte aligned.
- The trampoline (`process_trampoline()`) calls the process entry function and
  invokes `process_exit()` on return.

### 5. Trap & Syscall Handler (`trap.c`, `trap.S`)

Machine-mode trap handling:

- **`trap_vector`** (assembly in `trap.S`):
  - Saves all 32 integer registers plus `mcause`/`mepc`/`mstatus` into a
    `RiscVTrapFrame` (140 bytes) on the stack.
  - Calls `trap_handler(frame)`.
  - Restores registers and executes `mret`.
- **`trap_handler`** (C in `trap.c`):
  - On `ecall` (mcause=11): dispatches by syscall number in `a7`.
  - On any other exception: calls `panic()`.

**System calls (12 total):**

| # | Name    | Arguments               | Description                              |
|---|---------|-------------------------|------------------------------------------|
| 0 | YIELD   | —                       | Voluntarily yield the CPU               |
| 1 | EXIT    | —                       | Terminate the current process           |
| 2 | BLOCK   | —                       | Block the current process               |
| 3 | SLEEP   | `a0=ticks`             | Sleep for `ticks` scheduler iterations  |
| 4 | SEND    | `a0=pid, a1=type, a2=data, a3=len` | Send IPC message to process `pid` |
| 5 | RECV    | `a0=type, a1=buf, a2=len, a3=&sender` | Receive IPC message              |
| 6 | (reserved)| —                     | —                                       |
| 7 | OPEN    | `a0=path, a1=mode`     | Open/create a file (returns fd)         |
| 8 | READ    | `a0=fd, a1=buf, a2=len` | Read from a file                       |
| 9 | WRITE   | `a0=fd, a1=buf, a2=len` | Write to a file                        |
| 10| CLOSE   | `a0=fd`                | Close a file descriptor                 |
| 11| PUTC    | `a0=str, a1=len`       | Write string to UART console            |

### 6. UART Console (`console.c`)

- MMIO 16550-compatible UART at base address `0x10000000`.
- `console_putchar(c)`, `console_write(s, len)`, `console_puts(s)`.
- Formatted logging: `log_info`, `log_warn`, `log_error`, `panic` with
  `%s`, `%d`, `%u`, `%x`, `%p` format specifiers.

### 7. Driver Framework (`driver.c`)

A generic driver registration and I/O framework:

- `Driver` struct with callbacks: `init`, `open`, `close`, `write`, `read`, `ioctl`.
- `register_driver()` / `find_driver()` — linked-list registration.
- **Stage Buffer Pool** — pre-allocates a pool of `StageBuffer` structures with
  data buffers for asynchronous I/O:
  - `stage_pool_init(count, buf_size)` — initialise the pool.
  - `stage_alloc()` / `stage_free()` — acquire/release buffers.
  - `stage_submit(drv, buf)` — submit a buffer to a driver's `write` callback.

### 8. Emulated Flash (`flash.c`)

- In-RAM array (`flash_memory[65536]`) simulating flash storage.
- 256 blocks × 256 bytes = 64 KB total.
- `flash_read()`, `flash_write()`, `flash_erase()` — block-oriented operations.

### 9. FUSE Filesystem (`fuse.c`)

A simple flat filesystem layered on the emulated flash:

- **Superblock** (block 0): magic number (`0x53434653`), entry count, data start.
- **Directory entries**: up to 16 files, 24-character names, size, first block, mode.
- **File descriptors**: up to 16 open files simultaneously.
- **Operations**: `fuse_open`, `fuse_close`, `fuse_read`, `fuse_write`, `fuse_list`.
- Auto-formats the flash if no valid superblock is found.

### 10. ELF & Binary Loader (`loader.c`)

- `process_create(entry, arg)` — allocates a `Process` struct for a C function.
- `process_load_elf(elf_data, proc)` — validates a 32-bit RISC-V ELF header,
  loads `PT_LOAD` segments into heap-allocated memory, sets `PC` = ELF entry.
- `process_load_binary(data, size, entry, proc)` — loads a flat binary into
  heap memory with a specified entry point.

### 11. ABI (`abi/scorpion.h`)

Header for user-space programs compiled against the Scorpion kernel:

- Defines syscall numbers as macros.
- Provides inline-assembly wrappers for each syscall using RISC-V register
  conventions (`a7` = syscall number, arguments in `a0`–`a5`).

---

## RP2350 Platform Details

Scorpion targets the **Raspberry Pi RP2350** microcontroller in **RISC-V
mode**, where the two CPU cores implement the **Hazard3** RISC-V ISA
(RV32IMAC).

Key RP2350 features relevant to Scorpion:

| Feature          | Details                                  |
|------------------|------------------------------------------|
| **Cores**        | 2 × Hazard3 RISC-V (RV32IMAC + B‑ext)   |
| **SRAM**         | 512 KB (SRAM0–SRAM4, address `0x20000000`) |
| **XIP Flash**    | External QSPI flash via `0x10000000`     |
| **UART**         | 16550-compatible at `0x10000000`         |
| **Boot**         | ROM loads user binary from flash into SRAM or XIP |
| **Arch flags**   | `-march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -mabi=ilp32` |

The Pico SDK's RISC-V toolchain file (`cmake/preload/toolchains/pico_riscv_gcc.cmake`)
selects the `hazard3` system processor and uses the `B-extension` flags
(`zba`, `zbb`, `zbs`, `zbkb`) that the Hazard3 cores implement for
bit-manipulation instructions.

---

## Project Structure

```
├── CMakeLists.txt              # Build: uses Pico SDK for toolchain
├── README.md                   # This file
├── scorpion.h                  # Master kernel header (types, IPC, scheduler API)
├── main.c                      # Kernel entry: boots subsystems, creates processes
├── alloc.c / alloc.h            # Heap allocator (bump + free-list + buddy)
├── palloc.c                    # Physical page allocator (bitmap-based)
├── lifecycle.c                 # Process scheduler, yield, block, wake, kill
├── context.c                   # Context init, current_core_id(), trampoline
├── trap.c                      # Syscall dispatcher (ecall handler)
├── console.c / console.h        # UART driver + formatted logging
├── driver.c / driver.h          # Driver registration framework + stage buffers
├── flash.c / flash.h            # Emulated flash storage (in-RAM)
├── fuse.c / fuse.h              # FUSE filesystem on flash
├── loader.c                    # ELF & binary loader
├── cmake/
│   └── riscv32-toolchain.cmake   # Standalone toolchain file (reference)
├── arch/
│   └── riscv32/
│       ├── crt0.S                # C runtime: reset vector, data/BSS init
│       ├── context.S             # Context switch assembly
│       ├── trap.S                # Trap vector, trap handler wrapper
│       └── kernel.ld             # Linker script (SRAM layout)
├── abi/
│   └── scorpion.h               # User-space syscall wrappers
└── pico-sdk-2.3.0/              # Vendored Raspberry Pi Pico SDK v2.3.0
    └── ...                       # (used for toolchain configuration only)
```

---

## License

This project is provided for educational and development purposes. See
individual source files for license terms.
