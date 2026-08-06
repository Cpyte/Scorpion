#ifndef SCORPION_LOADER_H
#define SCORPION_LOADER_H

#include <stddef.h>
#include <stdint.h>

typedef struct Process Process;

typedef int (*loader_probe_fn)(const void *data, size_t size);
typedef int (*loader_load_fn)(const void *data, size_t size, Process *proc);

typedef struct LoaderFormat {
    const char *name;
    loader_probe_fn probe;
    loader_load_fn load;
} LoaderFormat;

int loader_register_format(const LoaderFormat *fmt);
int loader_load(const void *data, size_t size, Process *proc);
void loader_init(void);

int process_load_elf(const void *elf_data, size_t elf_size, Process *proc);
int process_load_binary(const void *data, size_t size,
                        uintptr_t entry_addr, Process *proc);

/* SEF v2 dynamic linking */
int process_load_lib(Process *proc, const void *data, size_t size);
void process_free_libs(Process *proc);

#endif
