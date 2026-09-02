#ifndef SCORPION_STRING_H
#define SCORPION_STRING_H

#include <stddef.h>
#include <stdint.h>

/*
 * Word-sized copies once both pointers are aligned to a machine word.
 * The byte prologue/epilogue keep the tail correct regardless of the
 * length, and the word loop turns the hot paths (flash RMW, IPC payload
 * copies, loader relocation) into roughly one load+store per 4 bytes
 * instead of one per byte.
 */

static inline void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    while (n > 0 && ((uintptr_t)d & (sizeof(uintptr_t) - 1u))) {
        *d++ = *s++;
        n--;
    }
    if (n > 0 && (((uintptr_t)s & (sizeof(uintptr_t) - 1u)) == 0)) {
        uintptr_t *wd = (uintptr_t *)d;
        const uintptr_t *ws = (const uintptr_t *)s;
        size_t words = n / sizeof(uintptr_t);

        n -= words * sizeof(uintptr_t);
        for (size_t i = 0; i < words; i++) {
            wd[i] = ws[i];
        }
        d = (unsigned char *)(wd + words);
        s = (const unsigned char *)(ws + words);
    }
    while (n > 0) {
        *d++ = *s++;
        n--;
    }

    return dest;
}

static inline void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;

    while (n > 0 && ((uintptr_t)p & (sizeof(uintptr_t) - 1u))) {
        *p++ = (unsigned char)c;
        n--;
    }
    if (n >= sizeof(uintptr_t)) {
        uintptr_t word = (unsigned char)c;
        word |= word << 8;
        word |= word << 16;
#if UINTPTR_MAX > 0xffffffffu
        word |= word << 32;
#endif
        uintptr_t *wp = (uintptr_t *)p;
        size_t words = n / sizeof(uintptr_t);

        for (size_t i = 0; i < words; i++) {
            wp[i] = word;
        }
        p = (unsigned char *)(wp + words);
        n -= words * sizeof(uintptr_t);
    }
    while (n > 0) {
        *p++ = (unsigned char)c;
        n--;
    }

    return s;
}

static inline int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];

        if (ca != cb) return (int)(ca - cb);
        if (ca == '\0') return 0;
    }
    return 0;
}

#endif
