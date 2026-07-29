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

#endif
