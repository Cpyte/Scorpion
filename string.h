#ifndef SCORPION_STRING_H
#define SCORPION_STRING_H

#include <stddef.h>

static inline void *memcpy(void *dest, const void *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        ((unsigned char *)dest)[i] = ((const unsigned char *)src)[i];
    }
    return dest;
}

static inline void *memset(void *s, int c, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        ((unsigned char *)s)[i] = (unsigned char)c;
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
