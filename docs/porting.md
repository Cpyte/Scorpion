# Porting Scorpion to a New Platform

Scorpion keeps every hardware dependency behind one header,
[`platform.h`](../platform.h). A port is:

1. a new directory `platform/<name>/` implementing that contract,
2. a linker script under the appropriate `arch/<isa>/`,
3. a branch in `CMakeLists.txt` selecting sources, flags and script,
4. an entry in [docs/platforms.md](platforms.md).

Nothing else changes: scheduler, allocators, loader, FUSE, IPC and the
syscall layer are platform-agnostic.

---

## 1. The platform contract

Implement each function in its own file (see `platform/rp2350/` for the
reference layout):

### Console

```c
void platform_console_init(void);   // called once from kernel_main()
void platform_uart_putc(char c);    // blocking single-byte output
```

`console.c` handles CRLF translation, `%d/%u/%x/%p/%s` formatting and
log prefixes; you only supply the byte pipe.

### Flash (optional but expected)

```c
int platform_flash_init(void);                                  // 0 / -1
int platform_flash_read_bytes(uint32_t off, void *buf, size_t len);
int platform_flash_erase_sector(uint32_t off);                  // 4 KB sector
int platform_flash_program_bytes(uint32_t off, const void *buf, size_t len);
```

Offsets are raw flash-device addresses. The generic `flash.c` layers
Scorpion's 256-byte blocks on top and performs the read-modify-write
that NOR flash needs; your program routine may assume the target range
is erased. If the platform cannot write flash yet, return `-1` from
every call — FUSE degrades gracefully (see `platform_payload_controller`
for the same pattern).

Block N is mapped to device offset
`FLASH_STORAGE_OFFSET + N * FLASH_BLOCK_SIZE`. Define
`FLASH_STORAGE_OFFSET` in `platform.h` for your platform so FUSE never
overlaps the boot image or any embedded payload (rp2350: `0x20000`,
esp32c3: `0x10000`).

### Timer

```c
uint64_t platform_timer_now(void);        // free-running counter
void     platform_timer_init(void);       // start counter + first arm
void     platform_timer_arm(uint64_t d);  // absolute compare deadline
bool     platform_timer_is_irq(uintptr_t mcause);
```

When the comparator matches, the CPU must take a trap whose `mcause`
equals the per-platform `PLATFORM_TIMER_MCAUSE` macro defined in
`platform.h`. The kernel's `timer_irq()` (in your timer file) re-arms
the compare and calls `yield()`, which is what makes preemption work.
Pick the interval so it is ~5–10 ms at your counter frequency.

### SMP (may be stubbed)

```c
void smp_launch_core1(void (*entry)(void), uintptr_t sp);
void core1_main(void);                    // entry body for core 1
```

Single-core platforms provide no-ops (see `platform/esp32c3/smp_stub.c`).
Multi-core platforms must bring the secondary core up with `mtvec`
already installed and a valid `sp`, then have it call
`scheduler_start_core()` after enabling its own timer interrupt.

### User-mode protection

```c
void pmp_init(void);   // declared in scorpion.h
```

On RISC-V platforms with PMP, fence the `_user_arena_start/_end` region
with TOR entries (`platform/rp2350/pmp.c`). Platforms without hardware
protection should log a warning about the reduced security model.

### Embedded application payload (optional)

```c
int platform_payload_controller(const uint8_t **data, uint32_t *size);
```

Returns the embedded controller SEF image that `init_process()` spawns
at boot, or `-1` if the platform has none. On rp2350 this reads the
`[size][sef]` blob written by `tools/packrom.py` through the XIP window.

---

## 2. Linker script

Add `arch/<isa>/kernel_<name>.ld`. Requirements:

* Export `_stack_top`, `_heap_start`, `_heap_end`,
  `_user_arena_start`, `_user_arena_end`, `_bss_start`, `_bss_end`,
  `_data_start`, `_data_end` — `crt0.S`, `alloc_init()`,
  `user_arena_init()` and `main.c` all consume these symbols.
* Keep the trap entry in `.text.trap` first-ish so vector alignment
  rules of your ISA are satisfied.
* If the image is RAM-resident and loaded directly by the chip's boot ROM
  (as on ESP32-C3/S3, which copy loadable segments themselves), set
  LMA == VMA; `crt0.S`'s data copy becomes a self-copy and `.bss`
  clearing still runs. See `arch/riscv32/kernel_esp32c3.ld`.

## 2b. The architecture layer (non-RISC-V ports)

If your chip is not RV32, you additionally provide `arch/<isa>/` with a
reset entry, a context switch for `CpuContext` and trap entry/exit
producing the shared `TrapFrame` — plus, in `scorpion.h`, that ISA's
block of intrinsics: exception-cause constants, `irq_save`/`irq_restore`,
`ARCH_IDLE()` and the `TRAP_*` macros that abstract where the syscall
number, arguments, return value and PC-advance live in a frame
(RV32: `a7`/`a0–a3`/`a0`/+4; Xtensa call0: `a2`/`a3–a6`/`a2`/+3).
Generic code touches none of these encodings directly. The ESP32-S3 port
(`arch/xtensa/`) is the worked example; timer interrupts are recognized
generically via `platform_timer_is_irq(frame->mcause)` rather than any
hard-coded cause number.

## 3. Build integration

In `CMakeLists.txt` add an `elseif(PLATFORM STREQUAL "<name>")` branch:
append your `platform/<name>/*.c` files to `KERNEL_SOURCES`, pick the
linker script, define `PLATFORM_<NAME>=1` (drives the `#ifdef` in
`platform.h`) and set `ARCH_FLAGS`. Toolchain selection must happen
before `project()`.

If your ISA is not RV32, also add `arch/<isa>/crt0.S`, `context.S`,
`trap.S` plus matching context/trap-frame structs — that is an
architecture port, not just a board port (Xtensa ESP32/S2/S3 fall in
this category).

## 4. Checklist

- [ ] `platform/<name>/` implements every prototype in `platform.h`
- [ ] `PLATFORM_TIMER_MCAUSE` matches what the trap handler sees
- [ ] Linker exports all reserved symbols above
- [ ] `cmake -B build -DPLATFORM=<name>` configures and builds clean
      with `-Wall -Wextra -Wpedantic`
- [ ] Boot prints `Scorpion kernel booting on <isa>` over console
- [ ] Preemption visible (test_user SEF round-robins)
- [ ] `pmp_init()` either fences user memory or warns loudly
- [ ] Entry added to docs/platforms.md support matrix
