# Scorpion Executable Formats

Scorpion uses a pluggable loader architecture (`loader.c`). The
`loader_load(data, size, proc)` function iterates registered format handlers
and dispatches to the first one whose `probe` routine returns success.

Two built-in formats are registered (in order):

1. **SEF** — Scorpion's native format
2. **ELF** — 32-bit RISC-V ELF

A third format, **BIN** (flat binary), is available only via the
`process_load_binary()` convenience function; it is **not** registered as a
general format handler since it cannot determine an entry point from the data.

---

## SEF (Scorpion Executable)

SEF is the recommended format for loading programs at runtime via
`scorpion_spawn()`. It is simple, self-contained, and does not require a
toolchain that targets the kernel's exact memory layout.

### Format

```
 Offset  Size  Field
 ──────  ────  ───────────────────────
   0       4    magic       = 0x00464553  ("SEF\0")
  4       4    entry       (byte offset from base of loaded segments)
  8       2    num_segments
 10       2    flags       (bit 0 = SEF_FLAG_PRIV_CONTROLLER)
 12      16×n  segments[]  (n = num_segments)

Each segment descriptor (16 bytes):
  Offset  Size  Field
  ──────  ────  ───────────────────────
   0       4    type        (0=TEXT, 1=DATA, 2=BSS)
   4       4    vaddr       (byte offset from base allocation)
   8       4    size        (byte length of segment)
  12       4    offset      (byte offset in the SEF file where data begins)

After the header + segment descriptors, raw segment data is concatenated at
the file offsets given by each segment's `offset` field.
```

### Building SEF

The `user/mksef.py` script converts a statically-linked RISC-V ELF into
SEF format:

```bash
python3 user/mksef.py input.elf output.sef
python3 user/mksef.py --flags 1 input.elf output.sef  # controller process
```

> **Note:** The ELF→SEF conversion path (`mksef.py`) is a development
> convenience and **not the recommended route** for production use. It
> relies on `objdump`, `readelf`, and `objcopy` to extract section data and
> does not preserve ELF metadata beyond the segment contents. For production,
> author SEF binaries directly using the format spec above, or use the ELF
> loader directly on hardware that supports XIP.

### Loading

The loader allocates one contiguous block of `total = sum(seg.size)` bytes,
copies TEXT/DATA segments from the file at their `vaddr` offsets, and
zero-fills BSS. The process `PC` is set to `base + entry`. The stack pointer
is placed at `base + total`.

### PackROM Integration

The `tools/packrom.py` script embeds a controller SEF binary into the UF2
image alongside the kernel. The kernel reads it from XIP flash at a fixed
offset during boot and spawns it via `scorpion_spawn`.

---

## ELF

Standard 32-bit RISC-V ELF (`e_machine = 0xF3`). The loader:

- Validates ELF class (32-bit), machine (RISC-V), and magic.
- Computes total memory needed across all `PT_LOAD` segments.
- Allocates one contiguous block and copies each `PT_LOAD` segment into it
  at the appropriate offset.
- Distinguishes text vs. data by ELF segment flags:
  - `PF_X` (1) → text
  - `PF_W` (2) → data
- Sets `PC` = `e_entry`, `SP` = `base + total_mem`.
- Does **not** set privilege (caller must set `proc->privilege`).

Use `loader_load(elf_data, elf_size, proc)` directly, or the convenience
wrapper `process_load_elf()`.

---

## BIN (Flat Binary)

The catch-all format accepted only when the data does not start with ELF or
SEF magic. The loader copies the data into a heap allocation and sets
`text_base`/`text_size`. No entry point, no stack, no privilege — the caller
must finish initialising the process context via `process_load_binary()`.

```c
int process_load_binary(const void *data, size_t size,
                        uintptr_t entry_addr, Process *proc);
```

---

## Adding a New Format

```c
static int my_probe(const void *data, size_t size);
static int my_load(const void *data, size_t size, Process *proc);

static const LoaderFormat my_format = {
    .name = "MYFMT",
    .probe = my_probe,
    .load = my_load,
};

loader_register_format(&my_format);
```

The `probe` function should return 0 on match, -1 on rejection. The `load`
function should allocate memory via `alloc_()`, set `proc->alloc_base` to the
allocation base address, fill in `proc->text_base/size`, `proc->data_base/size`,
`proc->bss_base/size` as appropriate, and initialise `proc->context`.
