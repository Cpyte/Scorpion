#include <stdint.h>
#include <string.h>

#include "alloc.h"
#include "console.h"
#include "loader.h"
#include "scorpion.h"

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

    const Elf32Phdr *phdr = (const Elf32Phdr *)((const uint8_t *)data + ehdr->phoff);

    for (unsigned i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != ELF_PT_LOAD) continue;
        if (phdr[i].memsz == 0) continue;

        uint8_t *seg = alloc_(phdr[i].memsz);
        if (seg == NULL) return -1;

        if (phdr[i].filesz > 0)
            memcpy(seg, (const uint8_t *)data + phdr[i].offset, phdr[i].filesz);
        if (phdr[i].memsz > phdr[i].filesz)
            memset(seg + phdr[i].filesz, 0, phdr[i].memsz - phdr[i].filesz);

        proc->stack_base = seg;
        proc->stack_size = phdr[i].memsz;
        break;
    }

    proc->context.pc = ehdr->entry;
    proc->context.ra = 0;
    proc->context.sp = ((uintptr_t)proc->stack_base + proc->stack_size) & ~(uintptr_t)0xF;
    for (unsigned i = 0; i < 12; i++) proc->context.s[i] = 0;
    proc->state = PROCESS_READY;
    proc->context_initialized = true;
    return 0;
}

static int bin_probe(const void *data, size_t size)
{
    (void)data;
    return (size > 0) ? 0 : -1;
}

static int bin_load(const void *data, size_t size, Process *proc)
{
    uint8_t *copy = alloc_(size);
    if (copy == NULL) return -1;

    memcpy(copy, data, size);
    proc->stack_base = copy;
    proc->stack_size = size;
    return 0;
}

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
    loader_register_format(&elf_format);
    loader_register_format(&binary_format);
}

int process_load_elf(const void *elf_data, Process *proc)
{
    return elf_load(elf_data, 0, proc);
}

int process_load_binary(const void *data, size_t size,
                         uintptr_t entry_addr, Process *proc)
{
    int ret = bin_load(data, size, proc);
    if (ret == 0) {
        proc->context.pc = entry_addr;
        proc->context.ra = 0;
        proc->context.sp = ((uintptr_t)proc->stack_base + proc->stack_size) & ~(uintptr_t)0xF;
        for (unsigned i = 0; i < 12; i++) proc->context.s[i] = 0;
        proc->context.gp = 0;
        proc->context.tp = 0;
        proc->state = PROCESS_READY;
        proc->context_initialized = true;
    }
    return ret;
}

Process *process_create(void (*entry)(void *), void *arg)
{
    Process *proc = alloc_(sizeof(Process));
    if (proc == NULL) return NULL;

    proc->pid = next_pid++;
    proc->state = PROCESS_UNUSED;
    proc->entry = entry;
    proc->wake_tick = 0;
    proc->msg_head = 0;
    proc->msg_tail = 0;
    proc->msg_count = 0;
    proc->argument = arg;
    proc->stack_base = NULL;
    proc->stack_size = 0;
    proc->next = NULL;
    proc->prev = NULL;
    proc->context_initialized = false;
    return proc;
}
