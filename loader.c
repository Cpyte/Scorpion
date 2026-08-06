#include <stdint.h>
#include <string.h>

#include "alloc.h"
#include "console.h"
#include "loader.h"
#include "scorpion.h"
#include "sef.h"

#define ELF_MAGIC      0x464C457F
#define ELF_32CLASS    1
#define ELF_RISCV      0xF3
#define ELF_PT_LOAD    1

#define MAX_FORMATS    8

/* Reserve this many bytes above the loaded segments for the process stack,
 * so stack growth never corrupts the code/data/BSS. */
#define USER_STACK_SIZE 4096u

typedef struct {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} Elf32Ehdr;

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} Elf32Phdr;

static const LoaderFormat *formats[MAX_FORMATS];
static unsigned format_count;

MemoryRegion kernel_region;
MemoryRegion controller_region;

static int elf_probe(const void *data, size_t size)
{
    if (size < 4) return -1;
    return (*(const uint32_t *)data == ELF_MAGIC) ? 0 : -1;
}

static int elf_load(const void *data, size_t size, Process *proc)
{
    const Elf32Ehdr *ehdr = (const Elf32Ehdr *)data;

    if (size < sizeof(Elf32Ehdr)) return -1;
    if (ehdr->ident[4] != ELF_32CLASS) return -1;
    if (ehdr->machine != ELF_RISCV) return -1;
    if (ehdr->phnum == 0) return -1;

    size_t ph_end = (size_t)ehdr->phoff + (size_t)ehdr->phnum * sizeof(Elf32Phdr);
    if (ph_end > size || ph_end < (size_t)ehdr->phoff) return -1;

    const Elf32Phdr *phdr = (const Elf32Phdr *)((const uint8_t *)data + ehdr->phoff);

    uint32_t first_vaddr = 0;
    uint32_t max_end = 0;
    bool have_load = false;
    for (unsigned i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != ELF_PT_LOAD) continue;
        if (phdr[i].memsz == 0) continue;
        if (phdr[i].vaddr + phdr[i].memsz < phdr[i].vaddr) return -1;
        if (!have_load || phdr[i].vaddr < first_vaddr) first_vaddr = phdr[i].vaddr;
        if (phdr[i].vaddr + phdr[i].memsz > max_end) max_end = phdr[i].vaddr + phdr[i].memsz;
        have_load = true;
    }
    if (!have_load) return -1;
    if (ehdr->entry < first_vaddr || ehdr->entry >= max_end) return -1;

    size_t total_mem = (size_t)(max_end - first_vaddr);

    uint8_t *base = ualloc_(total_mem + USER_STACK_SIZE);
    if (base == NULL) return -1;

    proc->alloc_base = (uintptr_t)base;
    proc->text_base = 0;
    proc->text_size = 0;
    proc->data_base = 0;
    proc->data_size = 0;
    proc->bss_base = 0;
    proc->bss_size = 0;

    for (unsigned i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != ELF_PT_LOAD) continue;
        if (phdr[i].memsz == 0) continue;

        if (phdr[i].offset + phdr[i].filesz > size ||
            phdr[i].offset + phdr[i].filesz < phdr[i].offset) {
            ufree_(base);
            return -1;
        }

        uint8_t *seg = base + (phdr[i].vaddr - first_vaddr);
        if (phdr[i].filesz > 0)
            memcpy(seg, (const uint8_t *)data + phdr[i].offset, phdr[i].filesz);
        if (phdr[i].memsz > phdr[i].filesz)
            memset(seg + phdr[i].filesz, 0, phdr[i].memsz - phdr[i].filesz);

        if (phdr[i].flags & 1) {
            proc->text_base = (uintptr_t)seg;
            proc->text_size = phdr[i].memsz;
        }
        if (phdr[i].flags & 2) {
            proc->data_base = (uintptr_t)seg;
            proc->data_size = phdr[i].memsz;
        }
    }

    proc->context.pc = (uintptr_t)base + (ehdr->entry - first_vaddr);
    proc->context.ra = 0;
    proc->context.sp = ((uintptr_t)base + total_mem + USER_STACK_SIZE) & ~(uintptr_t)0xF;
    proc->context.mstatus = (proc->privilege == PRIV_USER) ? 0x0088u : 0x1808u;
    proc->stack_size = USER_STACK_SIZE;
    for (unsigned i = 0; i < 12; i++) proc->context.s[i] = 0;
    proc->context.gp = 0;
    proc->context.tp = 0;

    proc->state = PROCESS_READY;
    proc->context_initialized = true;
    return 0;
}

static int sef_probe(const void *data, size_t size)
{
    if (size < 12) return -1;
    return (*(const uint32_t *)data == SEF_MAGIC) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* SEF v2: relocation + import/export binding (see docs/dynamic-linking.md) */

static int sef_load(const void *data, size_t size, Process *proc);

static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

static uint32_t rd_insn(const uint8_t *p)
{
    return rd_u32(p);
}

static void wr_insn(uint8_t *p, uint32_t v)
{
    wr_u32(p, v);
}

static void sef_patch_insn(uint8_t *p, uint32_t type, uint32_t addr)
{
    uint32_t insn = rd_insn(p);

    switch (type) {
    case SEF_R_HI20:
        insn = (insn & 0xfffffu) | ((addr >> 12) << 12);
        break;
    case SEF_R_LO12I:
        insn = (insn & 0xfffffu) | ((addr & 0xfffu) << 20);
        break;
    case SEF_R_LO12S: {
        uint32_t imm = addr & 0xfffu;
        insn = (insn & ~0xfe000f80u) |
               ((imm & 0x1fu) << 7) | (((imm >> 5) & 0x7fu) << 25);
        break;
    }
    case SEF_R_CALL: {
        int32_t disp = (int32_t)(addr - (uint32_t)(uintptr_t)p);
        uint32_t hi = (uint32_t)((disp + 0x800) >> 12) & 0xfffffu;
        uint32_t lo = (uint32_t)disp & 0xfffu;

        insn = (insn & 0xfffffu) | (hi << 12);
        wr_insn(p, insn);

        uint32_t jalr = rd_insn(p + 4);
        jalr = (jalr & 0xfffffu) | (lo << 20);
        wr_insn(p + 4, jalr);
        return;
    }
    default:
        return;
    }

    wr_insn(p, insn);
}

/* Write an absolute address into the image at [base + slot]. */
static void sef_write_addr(uint8_t *base, uint32_t slot, uint32_t type,
                           uint32_t addr)
{
    if (type == SEF_R_RELATIVE) {
        wr_u32(base + slot, addr);
    } else {
        sef_patch_insn(base + slot, type, addr);
    }
}

static int sef_apply_relocs(uint8_t *base, uint32_t total,
                            const uint8_t *data, size_t size,
                            uint32_t foff, uint32_t fsize)
{
    if (foff + fsize > size) return -1;

    const uint8_t *p = data + foff;
    const uint8_t *end = p + fsize;

    while (p + sizeof(SefRelocEntry) <= end) {
        uint32_t type   = rd_u32(p);
        uint32_t offset = rd_u32(p + 4);
        uint32_t value  = rd_u32(p + 8);
        uint32_t addr   = value + (uint32_t)base;

        if (offset + 4 > total) return -1;

        if (type == SEF_R_RELATIVE) {
            wr_u32(base + offset, addr);
        } else {
            sef_patch_insn(base + offset, type, addr);
        }
        p += sizeof(SefRelocEntry);
    }

    return (p == end) ? 0 : -1;
}

static char *sef_copy_name(const uint8_t *p, uint32_t name_len)
{
    char *name = alloc_(name_len + 1);
    if (name == NULL) return NULL;
    for (uint32_t i = 0; i < name_len; i++) name[i] = (char)p[i];
    name[name_len] = '\0';
    return name;
}

static int sef_parse_imports(Process *proc, const uint8_t *data, size_t size,
                             uint32_t foff, uint32_t fsize)
{
    if (foff + fsize > size) return -1;

    const uint8_t *p = data + foff;
    const uint8_t *end = p + fsize;
    uint32_t count = 0;

    while (p < end) {
        if (p + sizeof(SefImportRecord) > end) return -1;
        uint32_t name_len = rd_u32(p + 8);
        p += sizeof(SefImportRecord);
        if (p + name_len > end) return -1;

        count++;
        p += ALIGN_UP(name_len, 4);
        if (p > end) return -1;
    }

    if (count == 0) return 0;

    SefImportEntry *list = alloc_(count * sizeof(SefImportEntry));
    if (list == NULL) return -1;

    p = data + foff;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type    = rd_u32(p);
        uint32_t slot    = rd_u32(p + 4);
        uint32_t name_len = rd_u32(p + 8);
        const uint8_t *namep = p + sizeof(SefImportRecord);

        list[i].type = type;
        list[i].slot = slot;
        list[i].name_len = name_len;
        list[i].name = sef_copy_name(namep, name_len);
        if (list[i].name == NULL) {
            for (uint32_t j = 0; j < i; j++) free_(list[j].name);
            free_(list);
            return -1;
        }
        p += sizeof(SefImportRecord) + ALIGN_UP(name_len, 4);
    }

    proc->imports = list;
    proc->import_count = count;
    return 0;
}

static int sef_parse_exports(Process *proc, const uint8_t *data, size_t size,
                             uint32_t foff, uint32_t fsize)
{
    if (foff + fsize > size) return -1;

    const uint8_t *p = data + foff;
    const uint8_t *end = p + fsize;
    uint32_t count = 0;

    while (p < end) {
        if (p + sizeof(SefExportRecord) > end) return -1;
        uint32_t name_len = rd_u32(p + 4);
        p += sizeof(SefExportRecord);
        if (p + name_len > end) return -1;

        count++;
        p += ALIGN_UP(name_len, 4);
        if (p > end) return -1;
    }

    if (count == 0) return 0;

    SefExportEntry *list = alloc_(count * sizeof(SefExportEntry));
    if (list == NULL) return -1;

    p = data + foff;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t value    = rd_u32(p);
        uint32_t name_len = rd_u32(p + 4);
        const uint8_t *namep = p + sizeof(SefExportRecord);

        list[i].value = value;
        list[i].name_len = name_len;
        list[i].name = sef_copy_name(namep, name_len);
        if (list[i].name == NULL) {
            for (uint32_t j = 0; j < i; j++) free_(list[j].name);
            free_(list);
            return -1;
        }
        p += sizeof(SefExportRecord) + ALIGN_UP(name_len, 4);
    }

    proc->exports = list;
    proc->export_count = count;
    return 0;
}

static void sef_free_imports(SefImportEntry *list, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) free_(list[i].name);
    free_(list);
}

static void sef_free_exports(SefExportEntry *list, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) free_(list[i].name);
    free_(list);
}

/* Look up `name` among the app image's exports, then all loaded libs.
 * Returns the absolute address of the exported symbol, or NULL. */
static uint8_t *sef_lookup_export(Process *proc, const char *name,
                                  uint32_t name_len)
{
    for (uint32_t i = 0; i < proc->export_count; i++) {
        SefExportEntry *e = &proc->exports[i];
        if (e->name_len == name_len && strncmp(e->name, name, name_len) == 0)
            return (uint8_t *)proc->alloc_base + e->value;
    }
    for (uint16_t l = 0; l < proc->lib_count; l++) {
        LoadedLib *lib = &proc->libs[l];
        for (uint32_t i = 0; i < lib->export_count; i++) {
            SefExportEntry *e = &lib->exports[i];
            if (e->name_len == name_len && strncmp(e->name, name, name_len) == 0)
                return lib->base + e->value;
        }
    }
    return NULL;
}

/* Resolve every unresolved import of `img` against `proc`'s exports.
 * Returns true if any import was resolved this pass. */
static bool sef_bind_image(Process *proc, uint8_t *img_base,
                           SefImportEntry *imports, uint32_t import_count)
{
    bool progress = false;

    for (uint32_t i = 0; i < import_count; i++) {
        SefImportEntry *imp = &imports[i];
        uint8_t *target = sef_lookup_export(proc, imp->name, imp->name_len);
        if (target == NULL) continue;

        sef_write_addr(img_base, imp->slot, imp->type,
                       (uint32_t)(uintptr_t)target);
        progress = true;
    }

    return progress;
}

/* Iteratively bind the app image and every loaded lib until no import
 * resolves further (load order independent). */
static void sef_bind_all(Process *proc)
{
    bool progress;
    do {
        progress = sef_bind_image(proc, (uint8_t *)proc->alloc_base,
                                  proc->imports, proc->import_count);
        for (uint16_t l = 0; l < proc->lib_count; l++) {
            LoadedLib *lib = &proc->libs[l];
            if (sef_bind_image(proc, lib->base, lib->imports,
                               lib->import_count))
                progress = true;
        }
    } while (progress);
}

void process_free_libs(Process *proc)
{
    for (uint16_t i = 0; i < proc->lib_count; i++) {
        LoadedLib *lib = &proc->libs[i];
        if (lib->alloc_ptr) ufree_(lib->alloc_ptr);
        sef_free_imports(lib->imports, lib->import_count);
        sef_free_exports(lib->exports, lib->export_count);
        lib->alloc_ptr = NULL;
        lib->imports = NULL;
        lib->exports = NULL;
    }
    proc->lib_count = 0;

    sef_free_imports(proc->imports, proc->import_count);
    sef_free_exports(proc->exports, proc->export_count);
    proc->imports = NULL;
    proc->import_count = 0;
    proc->exports = NULL;
    proc->export_count = 0;
}

/* Load a dynamic SEF library into `proc` and bind imports/exports.
 * Returns the lib id (>= 0) or a negative error code. */
int process_load_lib(Process *proc, const void *data, size_t size)
{
    if (proc == NULL) return -1;

    Process scratch;
    memset(&scratch, 0, sizeof(scratch));
    scratch.privilege = PRIV_USER;

    if (sef_load(data, size, &scratch) != 0) return -1;
    uint16_t flags = (uint16_t)(rd_u32((const uint8_t *)data + 8) >> 16);
    if (!(scratch.context_initialized) || !(flags & SEF_FLAG_DYNAMIC)) {
        sef_free_imports(scratch.imports, scratch.import_count);
        sef_free_exports(scratch.exports, scratch.export_count);
        if (scratch.alloc_base) ufree_((void *)scratch.alloc_base);
        return -2;
    }
    if (scratch.import_count == 0 && scratch.export_count == 0) {
        sef_free_imports(scratch.imports, scratch.import_count);
        sef_free_exports(scratch.exports, scratch.export_count);
        if (scratch.alloc_base) ufree_((void *)scratch.alloc_base);
        return -3;
    }
    if (proc->lib_count >= SEF_MAX_LIBS) {
        sef_free_imports(scratch.imports, scratch.import_count);
        sef_free_exports(scratch.exports, scratch.export_count);
        if (scratch.alloc_base) ufree_((void *)scratch.alloc_base);
        return -4;
    }

    LoadedLib *lib = &proc->libs[proc->lib_count];
    lib->base = (uint8_t *)scratch.alloc_base;
    lib->alloc_ptr = (uint8_t *)scratch.alloc_base;
    lib->imports = scratch.imports;
    lib->import_count = scratch.import_count;
    lib->exports = scratch.exports;
    lib->export_count = scratch.export_count;
    proc->lib_count++;

    sef_bind_all(proc);

    return (int)proc->lib_count - 1;
}

static int sef_load(const void *data, size_t size, Process *proc)
{
    const uint8_t *d = (const uint8_t *)data;

    if (size < 12) return -1;
    if (rd_u32(d) != SEF_MAGIC) return -1;

    uint32_t hdr_words = rd_u32(d + 8);
    uint16_t num      = (uint16_t)(hdr_words & 0xffffu);
    uint16_t flags    = (uint16_t)(hdr_words >> 16);
    uint32_t entry    = rd_u32(d + 4);

    if (num > SEF_MAX_SEGMENTS) return -1;

    uint32_t segs_size = (uint32_t)num * sizeof(SefSegment);
    if (12u + segs_size > size) return -1;

    uint32_t total = 0;
    for (unsigned i = 0; i < num; i++) {
        uint32_t type = rd_u32(d + 12 + i * 16);
        if (type <= SEG_BSS)
            total += rd_u32(d + 12 + i * 16 + 8);
    }
    if (total == 0) return -1;

    uint8_t *base = ualloc_(total + USER_STACK_SIZE);
    if (base == NULL) return -1;

    proc->alloc_base = (uintptr_t)base;
    proc->text_base = 0;
    proc->text_size = 0;
    proc->data_base = 0;
    proc->data_size = 0;
    proc->bss_base = 0;
    proc->bss_size = 0;

    for (unsigned i = 0; i < num; i++) {
        uint32_t type   = rd_u32(d + 12 + i * 16);
        uint32_t vaddr  = rd_u32(d + 12 + i * 16 + 4);
        uint32_t ssize  = rd_u32(d + 12 + i * 16 + 8);
        uint32_t off    = rd_u32(d + 12 + i * 16 + 12);
        uint8_t *dest   = base + vaddr;

        if (type > SEG_BSS) continue;

        if (vaddr + ssize > total) {
            ufree_(base);
            return -1;
        }

        if (type == SEG_TEXT) {
            if (off + ssize > size) { ufree_(base); return -1; }
            memcpy(dest, d + off, ssize);
            proc->text_base = (uintptr_t)dest;
            proc->text_size = ssize;
        } else if (type == SEG_DATA) {
            if (off + ssize > size) { ufree_(base); return -1; }
            memcpy(dest, d + off, ssize);
            proc->data_base = (uintptr_t)dest;
            proc->data_size = ssize;
        } else if (type == SEG_BSS) {
            memset(dest, 0, ssize);
            proc->bss_base = (uintptr_t)dest;
            proc->bss_size = ssize;
        }
    }

    if (flags & SEF_FLAG_DYNAMIC) {
        for (unsigned i = 0; i < num; i++) {
            uint32_t type = rd_u32(d + 12 + i * 16);
            if (type != SEG_RELOC) continue;
            if (sef_apply_relocs(base, total, d, size,
                                 rd_u32(d + 12 + i * 16 + 12),
                                 rd_u32(d + 12 + i * 16 + 8)) != 0) {
                ufree_(base);
                return -1;
            }
        }
        for (unsigned i = 0; i < num; i++) {
            uint32_t type = rd_u32(d + 12 + i * 16);
            if (type != SEG_IMPORT) continue;
            if (sef_parse_imports(proc, d, size,
                                  rd_u32(d + 12 + i * 16 + 12),
                                  rd_u32(d + 12 + i * 16 + 8)) != 0) {
                ufree_(base);
                return -1;
            }
        }
        for (unsigned i = 0; i < num; i++) {
            uint32_t type = rd_u32(d + 12 + i * 16);
            if (type != SEG_EXPORT) continue;
            if (sef_parse_exports(proc, d, size,
                                  rd_u32(d + 12 + i * 16 + 12),
                                  rd_u32(d + 12 + i * 16 + 8)) != 0) {
                sef_free_imports(proc->imports, proc->import_count);
                proc->imports = NULL;
                proc->import_count = 0;
                ufree_(base);
                return -1;
            }
        }
    }

    proc->context.pc = (uintptr_t)base + entry;
    proc->context.gp = (uintptr_t)base;

    uintptr_t stack_top = (uintptr_t)base + total + USER_STACK_SIZE;
    stack_top &= ~(uintptr_t)0xF;
    proc->context.sp = stack_top;

    for (unsigned i = 0; i < 12; i++) proc->context.s[i] = 0;
    proc->context.ra = 0;
    proc->context.tp = 0;

    if (flags & SEF_FLAG_PRIV_CONTROLLER) {
        proc->privilege = PRIV_CONTROLLER;
    } else {
        proc->privilege = PRIV_USER;
    }
    proc->context.mstatus = (proc->privilege == PRIV_USER) ? 0x0088u : 0x1808u;
    proc->stack_size = USER_STACK_SIZE;

    proc->state = PROCESS_READY;
    proc->context_initialized = true;
    return 0;
}

static int bin_load(const void *data, size_t size, Process *proc)
{
    uint8_t *copy = ualloc_(size + USER_STACK_SIZE);
    if (copy == NULL) return -1;

    memcpy(copy, data, size);
    proc->alloc_base = (uintptr_t)copy;
    proc->text_base = (uintptr_t)copy;
    proc->text_size = size;
    proc->stack_size = USER_STACK_SIZE;
    proc->state = PROCESS_READY;
    proc->context_initialized = true;
    return 0;
}

static const LoaderFormat sef_format = {
    .name = "SEF",
    .probe = sef_probe,
    .load = sef_load,
};

static const LoaderFormat elf_format = {
    .name = "ELF",
    .probe = elf_probe,
    .load = elf_load,
};

/* bin_load is not registered as a general format — use process_load_binary() */

int loader_register_format(const LoaderFormat *fmt)
{
    if (format_count >= MAX_FORMATS) return -1;
    formats[format_count++] = fmt;
    return 0;
}

int loader_load(const void *data, size_t size, Process *proc)
{
    for (unsigned i = 0; i < format_count; i++) {
        if (formats[i]->probe(data, size) == 0) {
            log_info("loader: detected %s format", formats[i]->name);
            return formats[i]->load(data, size, proc);
        }
    }
    log_error("loader: no matching format");
    return -1;
}

void loader_init(void)
{
    loader_register_format(&sef_format);
    loader_register_format(&elf_format);
}

int process_load_elf(const void *elf_data, size_t elf_size, Process *proc)
{
    return elf_load(elf_data, elf_size, proc);
}

int process_load_binary(const void *data, size_t size,
                         uintptr_t entry_addr, Process *proc)
{
    int ret = bin_load(data, size, proc);
    if (ret == 0) {
        proc->context.pc = entry_addr;
        proc->context.ra = 0;
        proc->context.sp = ((uintptr_t)proc->text_base + proc->text_size +
                            USER_STACK_SIZE) & ~(uintptr_t)0xF;
        proc->context.mstatus = (proc->privilege == PRIV_USER) ? 0x0088u : 0x1808u;
        for (unsigned i = 0; i < 12; i++) proc->context.s[i] = 0;
        proc->context.gp = 0;
        proc->context.tp = 0;
    }
    return ret;
}

Process *process_alloc(void)
{
    Process *proc = alloc_(sizeof(Process));
    if (proc == NULL) return NULL;

    memset(proc, 0, sizeof(Process));
    proc->state = PROCESS_UNUSED;
    proc->privilege = PRIV_USER;
    spinlock_init(&proc->msg_lock);
    return proc;
}

Process *process_create(void (*entry)(void *), void *arg)
{
    Process *proc = process_alloc();
    if (proc == NULL) return NULL;

    proc->pid = next_pid++;
    proc->entry = entry;
    proc->argument = arg;
    return proc;
}
