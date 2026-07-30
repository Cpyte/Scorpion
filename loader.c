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
    (void)size;
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

    size_t total_mem = 0;
    for (unsigned i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != ELF_PT_LOAD) continue;
        total_mem += phdr[i].memsz;
    }
    if (total_mem == 0) return -1;

    uint8_t *base = alloc_(total_mem);
    if (base == NULL) return -1;

    proc->alloc_base = (uintptr_t)base;
    proc->text_base = 0;
    proc->text_size = 0;
    proc->data_base = 0;
    proc->data_size = 0;
    proc->bss_base = 0;
    proc->bss_size = 0;

    size_t offset = 0;
    for (unsigned i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != ELF_PT_LOAD) continue;
        if (phdr[i].memsz == 0) continue;

        if (phdr[i].offset + phdr[i].filesz > size ||
            phdr[i].offset + phdr[i].filesz < phdr[i].offset) {
            free_(base);
            return -1;
        }

        uint8_t *seg = base + offset;
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

        offset += phdr[i].memsz;
    }

    proc->context.pc = ehdr->entry;
    proc->context.ra = 0;
    proc->context.sp = ((uintptr_t)base + total_mem) & ~(uintptr_t)0xF;
    proc->context.mstatus = 0x1808u;
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

static int sef_load(const void *data, size_t size, Process *proc)
{
    const SefHeader *hdr = (const SefHeader *)data;

    if (size < 12) return -1;
    if (hdr->magic != SEF_MAGIC) return -1;

    unsigned num = hdr->num_segments;
    if (num > SEF_MAX_SEGMENTS) return -1;

    uint32_t segs_size = num * sizeof(SefSegment);
    if (12 + segs_size > size) return -1;

    uint32_t total = 0;
    for (unsigned i = 0; i < num; i++) {
        total += hdr->segments[i].size;
    }
    if (total == 0) return -1;

    uint8_t *base = alloc_(total);
    if (base == NULL) return -1;

    proc->alloc_base = (uintptr_t)base;
    proc->text_base = 0;
    proc->text_size = 0;
    proc->data_base = 0;
    proc->data_size = 0;
    proc->bss_base = 0;
    proc->bss_size = 0;

    for (unsigned i = 0; i < num; i++) {
        const SefSegment *seg = &hdr->segments[i];
        uint8_t *dest = base + seg->vaddr;

        if (seg->vaddr + seg->size > total) {
            free_(base);
            return -1;
        }

        if (seg->type == SEG_TEXT) {
            if (seg->offset + seg->size > size) { free_(base); return -1; }
            memcpy(dest, (const uint8_t *)data + seg->offset, seg->size);
            proc->text_base = (uintptr_t)dest;
            proc->text_size = seg->size;
        } else if (seg->type == SEG_DATA) {
            if (seg->offset + seg->size > size) { free_(base); return -1; }
            memcpy(dest, (const uint8_t *)data + seg->offset, seg->size);
            proc->data_base = (uintptr_t)dest;
            proc->data_size = seg->size;
        } else if (seg->type == SEG_BSS) {
            memset(dest, 0, seg->size);
            proc->bss_base = (uintptr_t)dest;
            proc->bss_size = seg->size;
        }
    }

    proc->context.pc = (uintptr_t)base + hdr->entry;
    proc->context.gp = (uintptr_t)base;

    uintptr_t stack_top = (uintptr_t)base + total;
    stack_top &= ~(uintptr_t)0xF;
    proc->context.sp = stack_top;

    for (unsigned i = 0; i < 12; i++) proc->context.s[i] = 0;
    proc->context.ra = 0;
    proc->context.tp = 0;
    proc->context.mstatus = 0x1808u;

    if (hdr->flags & SEF_FLAG_PRIV_CONTROLLER) {
        proc->privilege = PRIV_CONTROLLER;
    } else {
        proc->privilege = PRIV_USER;
    }

    proc->state = PROCESS_READY;
    proc->context_initialized = true;
    return 0;
}

static int bin_probe(const void *data, size_t size)
{
    if (size < 4) return -1;
    uint32_t magic = *(const uint32_t *)data;
    if (magic == ELF_MAGIC || magic == SEF_MAGIC) return -1;
    return (size > 0) ? 0 : -1;
}

static int bin_load(const void *data, size_t size, Process *proc)
{
    uint8_t *copy = alloc_(size);
    if (copy == NULL) return -1;

    memcpy(copy, data, size);
    proc->alloc_base = (uintptr_t)copy;
    proc->text_base = (uintptr_t)copy;
    proc->text_size = size;
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

static const LoaderFormat binary_format = {
    .name = "BIN",
    .probe = bin_probe,
    .load = bin_load,
};

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
    loader_register_format(&binary_format);
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
        proc->context.sp = ((uintptr_t)proc->text_base + proc->text_size) & ~(uintptr_t)0xF;
        proc->context.mstatus = 0x1808u;
        for (unsigned i = 0; i < 12; i++) proc->context.s[i] = 0;
        proc->context.gp = 0;
        proc->context.tp = 0;
        proc->state = PROCESS_READY;
        proc->context_initialized = true;
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
