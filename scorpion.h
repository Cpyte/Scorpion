#ifndef WEW_SCORPION_SCORPION_H
#define WEW_SCORPION_SCORPION_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sef.h"

#define IPC_MAX_DATA     64
#define IPC_QUEUE_DEPTH  16

#define HEAP_SIZE       (1024u * 1024u)
#define PAGE_SIZE       4096u
#define PAGE_SHIFT      12u
#define PAGE_MASK       (~(uintptr_t)(PAGE_SIZE - 1u))

#define MIN_ALLOC_SIZE  16u

#define ALIGN_UP(value, alignment) \
    (((value) + ((alignment) - 1u)) & ~((alignment) - 1u))

typedef struct {
    uint32_t type;
    uint8_t data[IPC_MAX_DATA];
    uint16_t sender_pid;
} Message;

typedef struct {
    atomic_bool locked;
} spinlock_t;

static inline void spinlock_init(spinlock_t *lock)
{
    atomic_store_explicit(&lock->locked, false, memory_order_relaxed);
}

static inline void spinlock_lock(spinlock_t *lock)
{
    while (atomic_exchange_explicit(&lock->locked, true, memory_order_acquire)) {
    }
}

static inline bool spinlock_try_lock(spinlock_t *lock)
{
    return !atomic_exchange_explicit(&lock->locked, true, memory_order_acquire);
}

static inline void spinlock_unlock(spinlock_t *lock)
{
    atomic_store_explicit(&lock->locked, false, memory_order_release);
}

typedef struct BlockHeader {
    size_t size;
    struct BlockHeader *next;
} BlockHeader;

#define ALLOC_HEADER_SIZE \
    ALIGN_UP(sizeof(BlockHeader), alignof(max_align_t))

typedef struct BuddyBlock {
    struct BuddyBlock *next;
} BuddyBlock;

#define MAX_ORDER 60u

#define BUDDY_STATE_FREE 0u
#define BUDDY_STATE_USED 1u

typedef struct {
    uint8_t order;
    uint8_t state;
} BuddyPage;

typedef enum {
    PALLOC_OK = 0,
    PALLOC_ERR_NOMEM,
    PALLOC_ERR_NOT_INITIALIZED,
    PALLOC_ERR_INVALID_ADDR,
    PALLOC_ERR_NOT_ALIGNED,
    PALLOC_ERR_ALREADY_FREE,
    PALLOC_ERR_NOT_FREE,
    PALLOC_ERR_INVALID_PARAM
} palloc_error_t;

palloc_error_t palloc_init(uintptr_t base, size_t size, uint8_t *bitmap, size_t bitmap_size);
palloc_error_t palloc_free_region(uintptr_t start, size_t count);
uintptr_t palloc(void);
uintptr_t palloc_contiguous(size_t count);
palloc_error_t pfree(uintptr_t addr);
palloc_error_t pfree_contiguous(uintptr_t addr, size_t count);
bool palloc_is_free(uintptr_t addr);
bool palloc_is_initialized(void);
size_t palloc_free_count(void);
size_t palloc_total_count(void);
palloc_error_t palloc_get_stats(size_t *total, size_t *free_out);
const char *palloc_strerror(palloc_error_t error);

#define PROCESS_STACK_SIZE 4096u

typedef enum {
    PRIV_KERNEL = 0,
    PRIV_CONTROLLER = 1,
    PRIV_USER = 2
} PrivilegeLevel;

typedef enum {
    PROCESS_UNUSED,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_TERMINATED
} ProcessState;

/* Per-CPU saved register context for a suspended thread. The exact
 * contents depend on the architecture — see the matching context.S.
 * Under Xtensa call0 only sp and a12–a15 are callee-saved; RV32 saves
 * the full conventional set. */
#if defined(__xtensa__)
typedef struct {
    uintptr_t pc;   /* resume address (= a0 at switch time) */
    uintptr_t sp;
    uintptr_t s[4]; /* a12–a15 */
} CpuContext;
#else
typedef struct {
    uintptr_t pc;
    uintptr_t ra;
    uintptr_t sp;
    uintptr_t gp;
    uintptr_t tp;
    uintptr_t s[12];
    uintptr_t mstatus;
} CpuContext;
#endif

typedef struct {
    uint32_t type;
    uint32_t slot;
    char    *name;
    uint32_t name_len;
} SefImportEntry;

typedef struct {
    uint32_t value;
    char    *name;
    uint32_t name_len;
} SefExportEntry;

typedef struct {
    uint8_t *base;
    uint16_t export_count;
    uint16_t import_count;
    SefExportEntry *exports;
    SefImportEntry *imports;
    uint8_t *alloc_ptr;
} LoadedLib;

typedef struct Process {
    uint32_t pid;
    ProcessState state;
    PrivilegeLevel privilege;
    uint8_t *stack_base;
    size_t stack_size;
    void (*entry)(void *argument);
    void *argument;
    struct Process *next;
    struct Process *prev;
    CpuContext context;
    bool context_initialized;
    uint32_t wake_tick;
    Message msg_queue[IPC_QUEUE_DEPTH];
    uint16_t msg_head;
    uint16_t msg_tail;
    uint16_t msg_count;
    uintptr_t alloc_base;
    uintptr_t text_base;
    size_t text_size;
    uintptr_t data_base;
    size_t data_size;
    uintptr_t bss_base;
    size_t bss_size;
    spinlock_t msg_lock;
    SefImportEntry *imports;
    uint16_t import_count;
    SefExportEntry *exports;
    uint16_t export_count;
    LoadedLib libs[SEF_MAX_LIBS];
    uint16_t lib_count;
} Process;

/* Full trap frame built by arch/<isa>/trap entry assembly before the C
 * handler runs. Field NAMES are shared across architectures on purpose:
 * generic code (trap.c) is written against them.
 *
 * RV32: standard machine frame. Xtensa: a0–a15 plus SAR/EXCVADDR, with
 * "mepc"=EPC1, "mstatus"=PS and a synthesized "mcause" — either an
 * exception's EXCCAUSE or 0x80000000|<cpu interrupt number> when the
 * entry came from an interrupt (matching how RISC-V encodes interrupts).
 */
#if defined(__xtensa__)
typedef struct {
    uint32_t a[16];
    uint32_t sar;
    uint32_t excvaddr;
    uint32_t mstatus;   /* PS at trap time */
    uint32_t mepc;      /* EPC1 */
    uint32_t mcause;    /* EXCCAUSE or 0x80000000|cpu_int */
} TrapFrame;
#else
typedef struct {
    uintptr_t mepc;
    uintptr_t ra;
    uintptr_t sp;
    uintptr_t gp;
    uintptr_t tp;
    uintptr_t t[7];
    uintptr_t s[12];
    uintptr_t a[8];
    uintptr_t mcause;
    uintptr_t mepc_saved;
    uintptr_t mstatus;
} TrapFrame;
#endif

/*
 * Syscall argument/return conventions differ per architecture. Generic
 * syscall dispatch goes through these macros so trap.c stays shared:
 *
 *   RV32:   sysno in a7, args a0–a3, return value in a0, ecall = 4 bytes
 *   Xtensa: sysno in a2, args a3–a6, return value in a2, syscall insn
 *           is 3 bytes long
 */
#if defined(__xtensa__)
#define TRAP_SYSCALL_NO(frame)  ((frame)->a[2])
#define TRAP_ARG(frame, n)      ((frame)->a[(n) + 3])
#define TRAP_RET(frame, v) \
    ((void)((frame)->a[2] = (uint32_t)(uintptr_t)(v)))
#define TRAP_ADVANCE_PC(frame)  ((void)((frame)->mepc += 3))
#else
#define TRAP_SYSCALL_NO(frame)  ((frame)->a[7])
#define TRAP_ARG(frame, n)      ((frame)->a[n])
#define TRAP_RET(frame, v) \
    ((void)((frame)->a[0] = (uintptr_t)(v)))
#define TRAP_ADVANCE_PC(frame)  ((void)((frame)->mepc += 4))
#endif

/*
 * "Did this trap come from user mode?" — used to decide whether a CPU
 * fault should terminate the offending process instead of panicking.
 *   RV32:   MPP[12:11] == 00 means U-mode
 *   Xtensa: PS.UM (bit 5) set means user ring
 */
#if defined(__xtensa__)
#define TRAP_IN_USER_MODE(frame) (((frame)->mstatus & 0x20u) != 0)
#else
#define TRAP_IN_USER_MODE(frame) (((frame)->mstatus & 0x1800u) == 0)
#endif

void context_switch(CpuContext *old, CpuContext *next);

#define MAX_CORES 4u

unsigned current_core_id(void);
void smp_launch_core1(void (*entry)(void), uintptr_t sp);
void core1_main(void);

extern Process *current_process[MAX_CORES];

void process_init_context(Process *process);
void process_exit(void);

extern uint32_t tick_count;
extern uint32_t next_pid;

void yield(void);
void block_process(void);
void wake_process(Process *p);
void sleep_process(uint32_t ticks);
void wake_sleeping_processes(void);
Process *process_by_pid(uint32_t pid);
int send_message(Process *target, uint32_t type, const void *data, size_t len);
int receive_message(uint32_t *type, void *buf, size_t len, uint16_t *sender_pid);

typedef struct ProcessNode {
    Process *process;
    uint16_t priority;
    struct ProcessNode *next;
    struct ProcessNode *prev;
} ProcessNode;

typedef struct {
    ProcessNode *head;
    ProcessNode *tail;
} ProcessQueue;

typedef struct {
    CpuContext context;
    uint8_t stack[PROCESS_STACK_SIZE];
} CoreScheduler;

extern CoreScheduler scheduler[MAX_CORES];

void scheduler_init(void);
void scheduler_start(void);
void scheduler_entry(void);
void scheduler_start_core(void);

void timer_enable(void);

void add_process(Process *proc, uint16_t priority);
int runprocess(void);
int killprocess(void);

void trap_handler(TrapFrame *frame);
void trap_init(void);

void console_init(void);
void console_putchar(char c);
void console_write(const char *s, size_t len);
void console_puts(const char *s);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);
void __attribute__((noreturn)) panic(const char *fmt, ...);

void user_arena_init(void);
void *ualloc_(size_t size);
void ufree_(void *ptr);

void pmp_init(void);

Process *process_alloc(void);
Process *process_create(void (*entry)(void *), void *arg);

int process_terminate(Process *proc);

typedef struct {
    uintptr_t base;
    size_t size;
} MemoryRegion;

extern MemoryRegion kernel_region;
extern MemoryRegion controller_region;

int evict_lowest_priority(size_t min_freed);

void process_init(void);

void timer_init(void);
void timer_irq(void);

/* ------------------------------------------------------------------ *
 * Architecture intrinsics: CSR access, interrupt masking, exception
 * cause constants and the idle instruction. Everything below is
 * per-ISA; generic code only uses the names, never the encodings.
 * ------------------------------------------------------------------ */

#if defined(__xtensa__)

/*
 * Xtensa (ESP32-S3 LX7), built with the call0 ABI (no register
 * windows): WOE stays off for the whole kernel, so window overflow/
 * underflow vectors can never fire.
 */

#define MCAUSE_ECALL_U     1u    /* EXCCAUSE_SYSCALL (we run ring 0) */
#define MCAUSE_ECALL_M     1u
#define MCAUSE_INSN_FAULT  0u    /* EXCCAUSE_ILLEGAL */
#define MCAUSE_LOAD_FAULT  3u    /* EXCCAUSE_LOADSTORE_ERROR */
#define MCAUSE_STORE_FAULT 3u
#define MCAUSE_INSN_PF     2u    /* EXCCAUSE_INSTR_ERROR */
#define MCAUSE_LOAD_PF     20u   /* EXCCAUSE_LOAD_PROHIBITED */
#define MCAUSE_STORE_PF    28u   /* EXCCAUSE_STORE_PROHIBITED */

static inline uint32_t irq_save(void)
{
    uint32_t ps;
    /* rsil atomically returns the old PS while raising INTLEVEL to 15. */
    __asm__ volatile ("rsil %0, 15" : "=r"(ps) : : "memory");
    return ps;
}

static inline void irq_restore(uint32_t ps)
{
    __asm__ volatile ("wsr.ps %0; rsync" : : "r"(ps) : "memory");
}

#define ARCH_IDLE() __asm__ volatile ("waiti 0")

#else /* !__xtensa__ — RV32 machine mode */

#define MCAUSE_ECALL_U      8u
#define MCAUSE_ECALL_M     11u
#define MCAUSE_INSN_FAULT   1u
#define MCAUSE_LOAD_FAULT   5u
#define MCAUSE_STORE_FAULT  7u
#define MCAUSE_INSN_PF     12u
#define MCAUSE_LOAD_PF     13u
#define MCAUSE_STORE_PF    15u

#define CSR_MSTATUS 0x300
#define CSR_MIE     0x304
#define CSR_MEPC    0x341
#define CSR_MCAUSE  0x342
#define CSR_MIP     0x344

#define csr_read(csr) ({ \
    uint32_t _v; \
    __asm__ volatile ("csrr %0, " csr : "=r"(_v)); \
    _v; \
})

#define csr_write(csr, val) ({ \
    __asm__ volatile ("csrw " csr ", %0" : : "r"(val)); \
})

static inline uint32_t irq_save(void)
{
    uint32_t mstatus;
    __asm__ volatile ("csrrci %0, mstatus, 8" : "=r"(mstatus) : : "memory");
    return mstatus;
}

static inline void irq_restore(uint32_t mstatus)
{
    __asm__ volatile ("csrw mstatus, %0" : : "r"(mstatus) : "memory");
}

#define ARCH_IDLE() __asm__ volatile ("wfi")

#endif /* __xtensa__ */

#endif
