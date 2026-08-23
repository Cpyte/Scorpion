# Scorpion Platforms

Scorpion's hardware-specific code is isolated behind the contract declared in
[`platform.h`](../platform.h). Everything else — scheduler, allocators,
loader, FUSE, syscalls — is shared. This page lists supported platforms,
their status and the details you need to bring one up.

## Support matrix

| Platform | Board | ISA            | Cores | Console | Timer/preempt | Flash/FUSE | SMP | User protection | Status       |
|----------|-------|----------------|-------|---------|---------------|------------|-----|-----------------|--------------|
| `rp2350` | Raspberry Pi Pico 2 | RV32IMAC(B) Hazard3 | 2 | UART0 (PL011) | SIO mtime/mtimecmp | bootrom QSPI + FUSE | SIO FIFO handshake | PMP TOR fence | **Stable** |
| `esp32c3` | ESP32-C3-DevKitM-1 etc. | RV32IMC | 1 | UART0 (ROM-configured) | SYSTIMER unit0 + INTC | ROM spiflash + FUSE | n/a | none (no PMP) | **Experimental** |
| `esp32s3` | ESP32-S3-DevKitC-1 etc. | Xtensa LX7 (call0) | 1 used of 2 | UART0 (ROM-configured) | SYSTIMER unit0 + int. matrix | ROM spiflash + FUSE | core 1 unused | none (ring 0 for all) | **Experimental** |

The ESP32-C3 and ESP32-S3 ports share the "RAM-run image loaded by the
boot ROM" model; the S3 additionally required an `arch/xtensa/` backend
(context switch, vector table, trap entry) alongside the RISC-V one —
see [Porting](porting.md).

---

## rp2350 — Raspberry Pi Pico 2 (stable)

The original target. Kernel boots from the UF2 image (boot2 + kernel +
controller SEF packed by `tools/packrom.py`), runs from SRAM at
`0x20000000`, reads/writes the external QSPI flash through the bootrom API
and stores the controller payload at flash offset `0x10000`.

```bash
cmake -B build . && cmake --build build
# hold BOOTSEL, plug USB, copy:
cp build/Scorpion.uf2 /Volumes/RP2350/
```

Details that live in this platform's sources:

| File                        | Responsibility                                        |
|-----------------------------|-------------------------------------------------------|
| `platform/rp2350/uart.c`    | PL011 @ `0x40070000`, GPIO0/1 funcsel, 115200 8n1     |
| `platform/rp2350/timer.c`   | SIO mtime/mtimecmp @ `0xd0000000`, MTIE, 6.7 ms tick  |
| `platform/rp2350/smp.c`     | Bootrom SIO-FIFO handshake to start core 1            |
| `platform/rp2350/flash.c`   | Bootrom function table lookup, XIP window, sector RMW |
| `platform/rp2350/pmp.c`     | PMP TOR entries fencing the user arena                |
| `platform/rp2350/payload.c` | Controller SEF located via XIP at offset `0x10000`    |

---

## esp32c3 (experimental)

Scorpion also runs on Espressif's RISC-V chips. The ESP32-C3 core is
RV32IMC in machine mode with standard CSRs (`mstatus`, `mtvec`, `mepc`,
`mcause`), so the generic RISC-V assembly under `arch/riscv32/` is reused
unchanged. What differs:

* **Boot / memory layout.** The C3 has no second-stage bootloader: the
  boot **ROM** validates an image header at flash offset `0x0` and copies
  its loadable segments into RAM itself before jumping to the entry point.
  `arch/riscv32/kernel_esp32c3.ld` places `.text/.rodata` in IRAM at
  `0x40380000` and `.data/.bss` in DRAM at `0x3FC90000`. Because every
  section has LMA == VMA, the ROM initialises `.data` itself and `crt0.S`'s
  copy step becomes a harmless self-copy; `.bss` is still cleared normally.
* **Console.** The boot ROM leaves UART0 running at 115200 8n1; Scorpion
  just polls the TX FIFO. No pin or clock configuration needed.
* **Preemption timer.** No CLINT on C-series silicon: SYSTIMER unit0
  counts at a fixed 16 MHz and TARGET0 raises an interrupt through the
  INTC (`0x600C2000`) as machine *external* interrupt. `timer_irq()`
  re-arms the comparator and acknowledges at both levels.
* **No A/B extensions.** RV32IMC means no `amoswap` and no bit-manipulation
  instructions: GCC calls libgcc helpers for atomics/popcount/clz.
  `platform/esp32c3/atomic_compat.c` provides `__atomic_exchange_1` using
  interrupt masking (sound because C3 has one HP core), and the build links
  `libgcc.a` explicitly.
* **Flash / FUSE.** The C3 has no memory-mapped flash window, so reads,
  erases and programs go through the legacy SPI-flash driver in mask ROM
  (`esp_rom_spiflash_read`/`_write`/`_erase_sector`). Entry points are
  fixed by silicon and were cross-checked against Espressif's generated
  `esp32c3.rom.ld` (group *spiflash_legacy*: `0x40000130`, `0x4000012c`,
  `0x40000128`). The ROM attached the flash chip while booting us, so its
  global chip state is already valid; interrupts are masked around every
  operation. `erase_sector()` takes a sector *number*, not an address.
  FUSE lives at device offset `FLASH_STORAGE_OFFSET = 0x10000` (64 KB),
  safely above the kernel image.
* **Single core.** `smp_launch_core1()` is a no-op stub; only scheduler
  core 0 exists.

### Build & flash

Requires any RV32-capable GCC (`riscv32-esp-elf-gcc` preferred; plain
`riscv64-elf-gcc` works too since it generates RV32 code) and
`esptool.py` (`pip install esptool`):

```bash
cmake -B build-c3 -DPLATFORM=esp32c3 .
cmake --build build-c3

esptool.py --chip esp32c3 --port /dev/ttyUSB0 --baud 921600 \
    write_flash 0x0 build-c3/Scorpion_esp32c3.bin
```

`tools/mkimage_esp32c3.py` wraps the kernel ELF into Espressif firmware
image format v1 (header + PT_LOAD segments + XOR checksum) automatically
as a post-build step.

### What works today

Kernel boot, console logging, process creation/scheduler, preemption,
IPC, syscalls, SEF/ELF loading into RAM, sleep/wake, flash-backed FUSE
filesystem (ROM spiflash driver). The controller SEF payload is still not
embedded: `platform_payload_controller()` fails gracefully and init logs
a warning.

### Security caveat

The C3 implements **no PMP**. User processes still run at MPP=0 and all
syscalls work, but out-of-arena accesses are not trapped by hardware —
the software range checks in `trap.c` remain the only guard. Do not run
untrusted code on this platform.

### Bring-up checklist (for maintainers)

A few constants were written from documentation rather than validated on
hardware. If the timer does not fire on your board, verify against the
ESP-IDF headers of your chip revision and adjust:

1. `PLATFORM_TIMER_MCAUSE` in `platform.h` — assumes SYSTIMER target0 =
   source **17**, routed to machine external interrupt
   (`soc/interrupts.h`). C6/H2/P4 use different numbers.
2. SYSTIMER register offsets in `platform/esp32c3/timer.c`
   (`soc/systimer_reg.h`): value/target/load/OP/INT register offsets and
   the UNIT0_OP enable/update bits.
3. INTC offsets: priority table base, threshold, EOI
   (`soc/intc_reg.h`).
4. UART0 STATUS layout if you change baud/clock config.

---

## esp32s3 (experimental)

The second Espressif target and the first non-RISC-V one. The ESP32-S3's
Xtensa LX7 runs the kernel with the **call0 ABI** (register windows are
never enabled, so window overflow/underflow vectors can't fire) — this
is what keeps a context switch down to `{pc, sp, a12–a15}`.

```bash
# toolchain: Espressif crosstool-NG release under ~/xtensa-tools/
# (macOS: codesign --force --sign - every binary after extracting)
cmake -B build-s3 -DPLATFORM=esp32s3 . && cmake --build build-s3

esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 \
    write_flash 0x0 build-s3/Scorpion_esp32s3.bin
```

| File | Responsibility |
|------|----------------|
| `arch/xtensa/crt0.S` | Mask/clear interrupts, set VECBASE, zero .bss, call main |
| `arch/xtensa/vectors.S` | VECBASE table (16×64 B slots), full-save trap entry, `rfe` exit, `ecall_trigger` |
| `arch/xtensa/context.S` | call0 context switch (`pc`=a0, callee-saved sp+a12–a15) |
| `arch/xtensa/trap.c` | `trap_init()` — re-asserts VECBASE |
| `arch/xtensa/runtime_helpers.c` | div/mod/clz/popcount + 64-bit shifts (no call0 libgcc multilib exists) |
| `arch/xtensa/kernel_esp32s3.ld` | IRAM `0x40370000` 448 K (vector table first), DRAM `0x3FC90000` 344 K |
| `platform/esp32s3/timer.c` | SYSTIMER target0 → **interrupt matrix source 57** → CPU int 2; mcause synthesized as `0x80000002` |
| `platform/esp32s3/flash.c` | ROM spiflash: erase `0x400009fc`, write `0x40000a14`, read `0x40000a20` |

Design notes:

- **Interrupt model**: peripheral sources are routed by the ROM helper
  `intr_matrix_set()` (S3 ROM `0x40001b54`) to CPU interrupt 2
  (priority level 1). Trap entry synthesizes an RISC-V-style mcause
  (`0x80000000 | cpu_int`) so generic `trap.c` dispatches through
  `platform_timer_is_irq()` identically on all platforms.
- **Trap return**: the restore sequence ends in `rfe`, which atomically
  reloads PC←EPC1 and PS←EPS1 with EXCM still set — there is no
  interrupt window during register restore at all.
- **DRAM ceiling**: the script stops DRAM at `0x3FCE5000` because the
  S3 ROM keeps `.data` variables around `0x3FCEFFxx–0x3FF1FFxx`
  (`ets_ops_table_ptr` etc.) that must not be overwritten.

### Security caveats

- All processes run in **ring 0**: Xtensa privileged instructions would
  fault from ring 1 (`wsr.ps`, `rsil` are ring-0 only), so the port
  currently does not use user rings. Software privilege checks in
  `trap.c` still gate every syscall.
- No MPU/MMU isolation of the user arena; core 1 is not scheduled.

### Bring-up checklist (unverified-on-hardware)

1. `ETS_SYSTIMER_TARGET0_INTR_SOURCE` = **57**
   (`soc/interrupts.h`) and CPU int 2 being level-1 in
   `platform/esp32s3/timer.c`.
2. SYSTIMER register offsets (shared C3/S3 TRM layout).
3. ROM spiflash addresses against `components/esp_rom/esp32s3/ld/
   esp32s3.rom.ld` for your IDF revision.
4. `mkimage_esp32c3.py --chip s3` emits the standard 24-byte header
   (main + extended, chip_id 9) — compare against `esptool.py
   image_info` if the ROM rejects the image.

---

## Adding a new platform

See [docs/porting.md](porting.md) — in short: create
`platform/<name>/` implementing the functions in `platform.h`, add a
linker script under `arch/<isa>/`, wire a `-DPLATFORM=<name>` branch in
`CMakeLists.txt`, and document it here.
