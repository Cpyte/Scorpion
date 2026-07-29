#ifndef SCORPION_ABI_H
#define SCORPION_ABI_H

#define SYS_YIELD  0
#define SYS_EXIT   1
#define SYS_BLOCK  2
#define SYS_WAKE   3
#define SYS_SLEEP  4
#define SYS_SEND   5
#define SYS_RECV   6
#define SYS_OPEN   7
#define SYS_READ   8
#define SYS_WRITE  9
#define SYS_CLOSE  10
#define SYS_PUTC   11

static inline void scorpion_yield(void)
{
    register unsigned a7 asm("a7") = SYS_YIELD;
    __asm__ volatile ("ecall" : : "r"(a7) : "memory");
}

static inline void scorpion_exit(void)
{
    register unsigned a7 asm("a7") = SYS_EXIT;
    __asm__ volatile ("ecall" : : "r"(a7) : "memory");
    for (;;) {}
}

static inline void scorpion_block(void)
{
    register unsigned a7 asm("a7") = SYS_BLOCK;
    __asm__ volatile ("ecall" : : "r"(a7) : "memory");
}

static inline void scorpion_sleep(unsigned ticks)
{
    register unsigned a0 asm("a0") = ticks;
    register unsigned a7 asm("a7") = SYS_SLEEP;
    __asm__ volatile ("ecall" : : "r"(a0), "r"(a7) : "memory");
}

static inline int scorpion_send(unsigned pid, unsigned type, const void *data, unsigned len)
{
    register unsigned a0 asm("a0") = pid;
    register unsigned a1 asm("a1") = type;
    register const void *a2 asm("a2") = data;
    register unsigned a3 asm("a3") = len;
    register unsigned a7 asm("a7") = SYS_SEND;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return (int)a0;
}

static inline int scorpion_recv(unsigned *type, void *buf, unsigned len, unsigned *sender_pid)
{
    register unsigned a0 asm("a0") = (unsigned)type;
    register void *a1 asm("a1") = buf;
    register unsigned a2 asm("a2") = len;
    register unsigned a3 asm("a3") = (unsigned)sender_pid;
    register unsigned a7 asm("a7") = SYS_RECV;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return (int)a0;
}

static inline void scorpion_wake(unsigned pid)
{
    register unsigned a0 asm("a0") = pid;
    register unsigned a7 asm("a7") = SYS_WAKE;
    __asm__ volatile ("ecall" : : "r"(a0), "r"(a7) : "memory");
}

static inline void scorpion_putc(const char *s, unsigned len)
{
    register const char *a0 asm("a0") = s;
    register unsigned a1 asm("a1") = len;
    register unsigned a7 asm("a7") = SYS_PUTC;
    __asm__ volatile ("ecall" : : "r"(a0), "r"(a1), "r"(a7) : "memory");
}

static inline int scorpion_open(const char *name, unsigned mode)
{
    register const char *a0 asm("a0") = name;
    register unsigned a1 asm("a1") = mode;
    register unsigned a7 asm("a7") = SYS_OPEN;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (int)(uintptr_t)a0;
}

static inline int scorpion_read(int fd, void *buf, unsigned size)
{
    register int a0 asm("a0") = fd;
    register void *a1 asm("a1") = buf;
    register unsigned a2 asm("a2") = size;
    register unsigned a7 asm("a7") = SYS_READ;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int scorpion_write(int fd, const void *buf, unsigned size)
{
    register int a0 asm("a0") = fd;
    register const void *a1 asm("a1") = buf;
    register unsigned a2 asm("a2") = size;
    register unsigned a7 asm("a7") = SYS_WRITE;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int scorpion_close(int fd)
{
    register int a0 asm("a0") = fd;
    register unsigned a7 asm("a7") = SYS_CLOSE;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

#endif
