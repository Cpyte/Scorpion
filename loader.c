#include <stdint.h>
#include <string.h>

#include "alloc.h"
#include "console.h"
#include "scorpion.h"

#define ELF_MAGIC      0x464C457F
#define ELF_32CLASS    1
#define ELF_RISCV      0xF3
#define ELF_PT_LOAD    1

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

static int load_elf_segments(const Elf32Ehdr *ehdr, Process *proc)
{
    const Elf32Phdr *phdr = (const Elf32Phdr *)((const uint8_t *)ehdr + ehdr->phoff);

    for (unsigned i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != ELF_PT_LOAD) {
            continue;
        }

        size_t seg_size = phdr[i].memsz;

        if (seg_size == 0) {
            continue;
        }

        proc->stack_base = alloc_(seg_size);

        if (proc->stack_base == NULL) {
            return -1;
        }

        proc->stack_size = seg_size;

        if (phdr[i].filesz > 0) {
            const uint8_t *src = (const uint8_t *)ehdr + phdr[i].offset;
            memcpy(proc->stack_base, src, phdr[i].filesz);
        }

        if (phdr[i].memsz > phdr[i].filesz) {
            memset(proc->stack_base + phdr[i].filesz, 0,
                   phdr[i].memsz - phdr[i].filesz);
        }

        break;
    }

    proc->context.pc = ehdr->entry;
    proc->context.ra = 0;

    uintptr_t stack_top = (uintptr_t)proc->stack_base + proc->stack_size;
    stack_top &= ~(uintptr_t)0xF;
    proc->context.sp = stack_top;

    for (unsigned i = 0; i < 12; i++) {
        proc->context.s[i] = 0;
    }

    proc->state = PROCESS_READY;
    proc->context_initialized = true;

    return 0;
}

Process *process_create(void (*entry)(void *), void *arg)
{
    Process *proc = alloc_(sizeof(Process));

    if (proc == NULL) {
        return NULL;
    }

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

int process_load_elf(const void *elf_data, Process *proc)
{
    const Elf32Ehdr *ehdr = (const Elf32Ehdr *)elf_data;

    if (*(const uint32_t *)elf_data != ELF_MAGIC) {
        log_error("loader: invalid ELF magic");
        return -1;
    }

    if (ehdr->ident[4] != ELF_32CLASS) {
        log_error("loader: not 32-bit ELF");
        return -1;
    }

    if (ehdr->machine != ELF_RISCV) {
        log_error("loader: not RISC-V");
        return -1;
    }

    if (ehdr->phnum == 0) {
        log_error("loader: no program headers");
        return -1;
    }

    return load_elf_segments(ehdr, proc);
}

int process_load_binary(const void *data, size_t size,
                         void (*entry)(void *), Process *proc)
{
    proc->stack_base = alloc_(size);

    if (proc->stack_base == NULL) {
        return -1;
    }

    memcpy(proc->stack_base, data, size);
    proc->stack_size = size;

    proc->entry = entry;
    proc->argument = NULL;
    proc->context.pc = (uintptr_t)entry;
    proc->context.ra = 0;

    uintptr_t stack_top = (uintptr_t)proc->stack_base + proc->stack_size;
    stack_top &= ~(uintptr_t)0xF;
    proc->context.sp = stack_top;

    for (unsigned i = 0; i < 12; i++) {
        proc->context.s[i] = 0;
    }

    proc->context.gp = 0;
    proc->context.tp = 0;
    proc->state = PROCESS_READY;
    proc->context_initialized = true;

    return 0;
}
