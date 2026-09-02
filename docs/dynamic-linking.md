# SEF v2 — Dynamic Linking (PIC + load-time relocation + import/export binding)

Status: kernel + toolchain implemented; hardware bring-up pending
Scope: Scorpion kernel (`WEW-scorpion`), Cpyte compiler (`WEW`), CPM (`WEW-package_manager`)

---

## 1. Problem

SEF v1 loads a single image into one arena allocation. The compiler pipeline
(`WEW/source/cpyte/compiling.py:488` `run_scorpion`) links at `-Ttext=0
--no-relax` and produces code that is **position-dependent**:

- Global data is accessed by absolute address (`lui/lw <link-time addr>`).
- Cross-function calls go through `.data.rel.ro` / GOT slots holding
  **link-time absolute function pointers**.
- The kernel loader (`loader.c:148` `sef_load`) copies images to an arena base
  at `0x2003E000+`, never to address 0.

Therefore **LLVM-generated SEFs cannot run correctly on the kernel today**; only
hand-written `auipc`-PIC assembly (`user/controller.S`, `user/test_user.S`)
works. This is the exact obstacle to dynamic linking: you cannot bind library
symbols into an image that itself assumes address 0.

## 2. Approach

Make user images **relocatable** and add a **load-time binding** stage to the
kernel (a miniature dynamic linker):

1. **PIC codegen**: llvmlite emits PIC objects (`reloc='pic'`) for `.cpy`
   code (`R_RISCV_GOT_HI20` / `R_RISCV_PCREL_HI20` verified); `runtime_scorpion.c`
   is compiled with `-fPIC`.
2. **Keep relocations**: the final link uses `-Wl,-q` (`--emit-relocs`) so the
   statically-linked ELF still carries its `.rela.*` tables.
3. **SEF v2 metadata**: a new `SEF_FLAG_DYNAMIC` image carries three extra
   segment kinds:
   - `SEG_RELOC` — absolute references that must have the load base added
     (`.data.rel.ro` pointers, GOT entries, and any remaining absolute
     instruction immediates).
   - `SEG_IMPORT` — references to symbols that are *not* defined in the image
     (resolved at runtime from loaded libraries).
   - `SEG_EXPORT` — the image's callable symbols (a library's ABI surface).
4. **Kernel**: after copying segments, apply the relocation pass; add a
   `SYS_LOADLIB` syscall that loads a library SEF into the calling process and
   binds imports↔exports.

Position-independent relocations (`PCREL_*`, `CALL`, `BRANCH`, `RVC_*`) are
skipped by the loader. No `-shared`/ET_DYN is needed: the bare-metal GNU ld
does not support it, and text lives in writable SRAM so instruction-immediate
relocation is fine.

## 3. SEF v2 format

### Header
Same 12-byte header as v1 (`sef.h`). New flag bit:

| Flag | Value | Meaning |
|------|-------|---------|
| `SEF_FLAG_PRIV_CONTROLLER` | `0x0001` | spawn as controller |
| `SEF_FLAG_DYNAMIC`         | `0x0002` | carries RELOC/IMPORT/EXPORT metadata |

### Segment types

| Type | Value | Kind | Contents |
|------|-------|------|----------|
| `SEG_TEXT`  | 0 | memory | code, mapped at `base+vaddr` |
| `SEG_DATA`  | 1 | memory | data, mapped at `base+vaddr` |
| `SEG_BSS`   | 2 | memory | zero-filled, mapped |
| `SEG_RELOC` | 3 | meta  | relocation records (not mapped) |
| `SEG_IMPORT`| 4 | meta  | import records (not mapped) |
| `SEG_EXPORT`| 5 | meta  | export records (not mapped) |

Metadata segments use `vaddr = 0`; the loader reads them from the file
(`offset`, `size`) and does **not** include them in the `total` allocation
(only TEXT/DATA/BSS are mapped).

### Relocation records (`SEG_RELOC`)
```
Offset  Size  Field
  0       4    type     (SEF_R_*)
  4       4    offset   (byte offset within the loaded image)
  8       4    value    (link-time address = symbol value + addend)
```
The loader writes `value + base`:
- `SEF_R_RELATIVE (0)`: `*(u32 *)(base + offset) = value + base` (data pointer)
- `SEF_R_HI20 (1)`: rewrite `lui` immediate to `(value + base) >> 12`
- `SEF_R_LO12I (2)`: rewrite `addi`/`lw` immediate to `(value + base) & 0xfff`
- `SEF_R_LO12S (3)`: rewrite `sw`/store immediate (shifted) to `(value+base)&0xfff`
- `SEF_R_CALL (4)`: patch the `auipc`+`jalr` pair at `offset` so it branches to
  `value + base`. Used for calls that the linker routed through a PLT stub
  (`.rela.text` `R_RISCV_CALL(_PLT)`), so the stub (which would read an
  uninitialised `.got.plt`) is bypassed and the call lands directly on the
  resolved symbol.

ELF mapping (host tool): `R_RISCV_32`→`SEF_R_RELATIVE`, `R_RISCV_HI20`→1,
`R_RISCV_LO12I`→2, `R_RISCV_LO12S`→3, `R_RISCV_CALL(_PLT)`→4. `SHN_ABS`
references are emitted as **no record** (fixed MMIO/flash addresses must not be
relocated). PC-relative relocations (`PCREL_*`, `BRANCH`, `JAL`, `RVC_*`,
`RELAX`, `ADD*`/`SUB*`, `32_PCREL`) are skipped: their displacement is
unchanged when the whole image moves together.

Note: the GNU ld resolves GOT slots for *defined* default-visibility symbols at
link time and drops the `.rela.got` entries. `elf2sef.py` therefore also scans
`.got`/`.got.plt` and emits a `SEF_R_RELATIVE` record for every slot whose word
equals a defined symbol address (skipping zero / `0xffffffff` PLT placeholders
and words that point into `.plt`).

### Import records (`SEG_IMPORT`)
```
Offset  Size  Field
  0       4    slot      (image offset of the 4-byte slot to fill)
  4       4    name_len
  8    name_len  name     (UTF-8, padded to 4-byte alignment)
```
At bind time the kernel resolves `name` against the process's own exports and
every loaded lib's exports and writes the resolved address into the slot:
- `SEF_R_RELATIVE`: the slot is a GOT entry / data word; store the absolute
  address.
- `SEF_R_HI20/LO12I/LO12S`: the slot is an instruction immediate.
- `SEF_R_CALL`: the slot is the `auipc` of a call; patch the pair to branch
  directly to the imported function.

Imports carry the same `SEF_R_*` type as the source relocation, so a GOT-based
external access (`R_RISCV_32`, undefined symbol) binds into the GOT slot, and an
external call (`R_RISCV_CALL_PLT`, undefined symbol) patches the call site.

### Export records (`SEG_EXPORT`)
```
Offset  Size  Field
  0       4    value     (image-relative offset of the exported symbol)
  4       4    name_len
  8    name_len  name     (UTF-8, padded to 4 bytes)
```

## 4. Kernel loader

`sef_load` gains a v2 path:

1. Parse header; sum TEXT/DATA/BSS sizes → `total`; allocate
   `ualloc_(total + USER_STACK_SIZE)` as today.
2. Map TEXT/DATA/BSS.
3. If `SEF_FLAG_DYNAMIC`:
   - parse `SEG_RELOC` → apply each record (relocation pass).
   - parse `SEG_IMPORT` → copy records into a kernel-owned list on the Process.
   - parse `SEG_EXPORT` → copy records into a kernel-owned list.
   - `pc = base + entry`, `sp` as today.

### Process additions (`scorpion.h`)
```c
typedef struct {
    uint32_t slot;
    char    *name;      // kernel heap copy
    uint32_t name_len;
} SefImportEntry;

typedef struct {
    uint32_t value;
    char    *name;
    uint32_t name_len;
} SefExportEntry;

typedef struct {
    uint8_t *base;                  // lib image base in user arena
    uint16_t export_count;
    uint16_t import_count;
    SefExportEntry *exports;
    SefImportEntry *imports;
    uint8_t *alloc_ptr;             // what to ufree_() on teardown
} LoadedLib;

Process:
    SefImportEntry *imports; uint16_t import_count;  // app image imports
    SefExportEntry *exports;  uint16_t export_count;  // app image exports
    LoadedLib libs[SEF_MAX_LIBS]; uint16_t lib_count; // per-process libs
```
All name strings and record arrays live in **kernel heap** (`alloc_`/`free_`),
so binding does not touch user memory.

## 5. `SYS_LOADLIB` (syscall 14)

```
a0 = sef_data (user ptr, validated)   a1 = size
returns lib id (>= 0) or negative error
```

Flow (`trap.c`):
1. Validate caller is a user/controller process and `sef_data`/`size` are in
   user range.
2. `loader_load` a **new scratch Process** to get the lib image + its
   import/export tables (reuses all v2 logic; the lib never enters the queue).
3. If the image is not `SEF_FLAG_DYNAMIC`, reject (`-2`).
4. Register `LoadedLib` in the caller's `libs[]` (evict/error if full).
5. **Bind**:
   - resolve the lib's imports against the caller's exports and all existing
     libs' exports (dependency chains);
   - resolve the caller's imports against this lib's exports.
   Repeat until no progress to make load order irrelevant.
6. Return the lib id (index into `libs[]`).

Permissions: any process may load a library into itself (it is the process's
own memory). `SYS_SPAWN` remains controller-only.

## 6. Teardown

`killprocess()` / `process_terminate()` additionally `ufree_` each `libs[i]`
allocation and `free_` the import/export name arrays and record arrays.

## 7. Toolchain

### Compiler (`WEW/source/cpyte/compiling.py` `run_scorpion`)
- compile `runtime_scorpion.c` with `-fPIC` when `pic=True`;
- link with `-Wl,-q` (`--emit-relocs`) and `-Wl,--unresolved-symbols=ignore-all`
  when `pic=True` (so undefined externals become `SEG_IMPORT` instead of
  hard link errors);
- invoke `WEW-scorpion/tools/elf2sef.py` instead of `mksef.py` when `pic=True`,
  passing `--export` names for libraries (`exports=[...]`);
- `cpy --scorpion --pic` selects the dynamic path (wired in `mainpie.py`).

### `tools/elf2sef.py` (host, pure Python)
Given a `--emit-relocs` ELF and an export list:
- extract the flat load image (`.text/.rodata/.eh_frame/.data.rel.ro/.got/.got.plt/.data/.bss`);
- walk `.rela.*` of loadable sections only (`sh_info` target is `SHF_ALLOC`);
  emit `SEF_R_*` records for absolute relocations (`R_RISCV_32`, `R_RISCV_HI20`,
  `R_RISCV_LO12_I/S` when paired with a `lui`, `R_RISCV_CALL(_PLT)`) against
  **defined** symbols; emit `SEG_IMPORT` for **undefined** symbols; skip all
  PC-relative/branch/RVC/relax/debug relocations;
- scan `.got`/`.got.plt` for linker-resolved slots (see §3) and emit
  `SEF_R_RELATIVE` records;
- emit `SEG_EXPORT` for the requested `--export NAME` symbols;
- produce the SEF v2 binary directly (authoring SEF directly is the sanctioned
  path per `docs/exec-format.md`).

Status: implemented and host-verified against an llvmlite PIC ELF (all call
patches and GOT/`.data.rel.ro` slots relocated correctly; undefined-symbol
imports emitted as `SEG_IMPORT`).

## 8. Verification (host-side)

Done during implementation, without hardware:

1. **Relocation correctness**: `elf2sef.py` output for an llvmlite PIC ELF and
   a real `cpy --scorpion --pic` build was loaded into a Python re-implementation
   of the kernel's relocation pass at a simulated arena base (`0x2003E000`):
   every `SEF_R_CALL` pair landed on `base + <symbol>` (including backward
   calls) and every `SEF_R_RELATIVE` slot (`.got`, `.data.rel.ro`) held
   `base + <link-time value>`.
2. **Import path**: a synthetic ELF with undefined `foo`/`bar` produced
   `SEG_IMPORT` records (CALL type) plus a `SEG_RELOC` for a defined data
   symbol; a real WEW build imported `bigint_from_str`/`bigint_print` via GOT
   slots (RELATIVE type).
3. **Kernel build**: `SEF_FLAG_DYNAMIC` load path compiles and links in the
   normal build.

## 9. Limits & notes

- No MMU: libraries are not shared between processes; each process loads its
  own copy. The payoff is separate artifact storage (flash), per-process
  version selection (CPM's scoped-version model), and load-time symbol
  resolution.
- The linker resolves GOT slots of defined globals at link time; `elf2sef.py`
  recovers them by scanning `.got` (see §3). GOT entries for *undefined*
  symbols resolve to zero and are handled only when the linker also emits the
  corresponding `R_RISCV_32` in `.rela.got` — if it does not, that import is
  silently missed (v1 limitation; in practice the WEW runtime is linked in and
  externals are only syscalls).
- The loader trusts the image the controller passes to `SYS_LOADLIB`/`SYS_SPAWN`
  (the same trust model as SPAWN) but validates the metadata it acts on: every
  import `slot` is bounds-checked against the mapped image size at parse time
  (`sef_import_slot_valid` in `loader.c`), so a slot (plus the write extent of
  its relocation type: 4 bytes, or 8 for `SEF_R_CALL`) must stay within the
  image — an out-of-range slot fails the load. Export `value`s are only used as
  the *target* of an address write into an already-validated slot and so cannot
  be abused as a write primitive.
- Forward references between libs are handled by iterative binding (load order
  independent).
- Max per-process libs: `SEF_MAX_LIBS` (default 4). Import/export counts are
  bounded by memory, not format.
- libgcc (`-lgcc`) is non-PIC; any absolute refs it contributes are covered by
  `SEF_R_HI20/LO12*` records and relocated like everything else.
