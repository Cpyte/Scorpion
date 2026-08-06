#include "scorpion.h"
#include <string.h>

#define BITMAP_BITS 8

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

typedef struct {
    uint8_t *bitmap;
    size_t   bitmap_entries;
    size_t   total_pages;
    size_t   free_pages;
    uintptr_t base;
    size_t   region_size;
    spinlock_t lock;
    bool     initialized;
} palloc_state_t;

static palloc_state_t palloc_state = {0};

static inline bool is_page_aligned(uintptr_t addr) {
    return (addr & (PAGE_SIZE - 1)) == 0;
}

static inline bool is_in_range(uintptr_t addr) {
    return addr >= palloc_state.base &&
           addr < palloc_state.base + palloc_state.region_size;
}

static inline size_t addr_to_page(uintptr_t addr) {
    return (size_t)((addr - palloc_state.base) >> PAGE_SHIFT);
}

static inline uintptr_t page_to_addr(size_t page) {
    return palloc_state.base + ((uintptr_t)page << PAGE_SHIFT);
}

static inline size_t page_to_byte(size_t page) {
    return page >> 3;
}

static inline size_t page_to_bit(size_t page) {
    return page & (BITMAP_BITS - 1);
}

static inline bool bitmap_test(const uint8_t *bitmap, size_t page) {
    return (bitmap[page_to_byte(page)] >> page_to_bit(page)) & 1;
}

static inline void bitmap_set_used(uint8_t *bitmap, size_t page) {
    bitmap[page_to_byte(page)] |= (uint8_t)(1u << page_to_bit(page));
}

static inline void bitmap_set_free(uint8_t *bitmap, size_t page) {
    bitmap[page_to_byte(page)] &= (uint8_t)~(1u << page_to_bit(page));
}

/* Bitmask of the pages of `byte_idx` that lie in [first, end_page). */
static inline uint8_t byte_range_mask(size_t first, size_t end_page,
                                      size_t byte_idx, size_t *next) {
    const size_t byte_start = byte_idx * BITMAP_BITS;
    const size_t byte_end   = byte_start + BITMAP_BITS;
    const size_t lo = first - byte_start;
    const size_t hi = (end_page <= byte_end) ? (end_page - 1u) - byte_start
                                             : BITMAP_BITS - 1u;
    *next = (byte_end < end_page) ? byte_end : end_page;
    return (uint8_t)((uint8_t)(0xFFu << lo) &
                     (uint8_t)(0xFFu >> (7u - hi)));
}

palloc_error_t palloc_init(uintptr_t base, size_t size,
                           uint8_t *bitmap, size_t bitmap_size) {
    if (unlikely(!is_page_aligned(base))) {
        return PALLOC_ERR_NOT_ALIGNED;
    }

    if (unlikely(size < PAGE_SIZE)) {
        return PALLOC_ERR_NOMEM;
    }

    if (unlikely(bitmap == NULL || bitmap_size == 0)) {
        return PALLOC_ERR_INVALID_PARAM;
    }

    size = (size / PAGE_SIZE) * PAGE_SIZE;
    size_t total = size >> PAGE_SHIFT;

    size_t required = (total + BITMAP_BITS - 1) / BITMAP_BITS;
    if (unlikely(bitmap_size < required)) {
        return PALLOC_ERR_NOMEM;
    }

    memset(bitmap, 0xFF, required);

    palloc_state.bitmap         = bitmap;
    palloc_state.bitmap_entries = required;
    palloc_state.total_pages    = total;
    palloc_state.free_pages     = 0;
    palloc_state.base           = base;
    palloc_state.region_size    = size;
    palloc_state.initialized    = false;

    spinlock_init(&palloc_state.lock);
    palloc_state.initialized = true;

    return PALLOC_OK;
}

palloc_error_t palloc_free_region(uintptr_t start, size_t count) {
    if (unlikely(!palloc_state.initialized)) {
        return PALLOC_ERR_NOT_INITIALIZED;
    }

    if (unlikely(!is_page_aligned(start))) {
        return PALLOC_ERR_NOT_ALIGNED;
    }

    if (count == 0) {
        return PALLOC_OK;
    }

    if (unlikely(start + count * PAGE_SIZE < start)) {
        return PALLOC_ERR_INVALID_ADDR;
    }

    const size_t start_page = addr_to_page(start);
    const size_t end_page   = start_page + count;

    if (unlikely(end_page > palloc_state.total_pages || !is_in_range(start))) {
        return PALLOC_ERR_INVALID_ADDR;
    }

    spinlock_lock(&palloc_state.lock);

    uint8_t *const bitmap = palloc_state.bitmap;
    size_t free_pages     = palloc_state.free_pages;
    size_t i              = start_page;

    while (i < end_page) {
        const size_t byte_idx = page_to_byte(i);
        size_t next;
        const uint8_t mask = byte_range_mask(i, end_page, byte_idx, &next);
        const uint8_t byte = bitmap[byte_idx];

        if (unlikely((byte & mask) != mask)) {
            /* Mixed byte: fall back to exact per-page semantics. */
            for (; i < end_page; i++) {
                if (unlikely(!bitmap_test(bitmap, i))) {
                    palloc_state.free_pages = free_pages;
                    spinlock_unlock(&palloc_state.lock);
                    return PALLOC_ERR_ALREADY_FREE;
                }
                bitmap_set_free(bitmap, i);
                free_pages++;
            }
            break;
        }

        bitmap[byte_idx] = (uint8_t)(byte & ~mask);
        free_pages      += (size_t)__builtin_popcount(byte & mask);
        i = next;
    }

    palloc_state.free_pages = free_pages;
    spinlock_unlock(&palloc_state.lock);
    return PALLOC_OK;
}

uintptr_t palloc(void) {
    return palloc_contiguous(1);
}

uintptr_t palloc_contiguous(size_t n) {
    if (unlikely(!palloc_state.initialized || n == 0)) {
        return 0;
    }

    spinlock_lock(&palloc_state.lock);

    if (unlikely(palloc_state.free_pages < n)) {
        spinlock_unlock(&palloc_state.lock);
        return 0;
    }

    const uint8_t *bitmap = palloc_state.bitmap;
    const size_t   total  = palloc_state.total_pages;
    const uintptr_t base  = palloc_state.base;

    size_t run   = 0;
    size_t start = 0;

    for (size_t i = 0; i < total; i += BITMAP_BITS) {
        const uint8_t byte = bitmap[page_to_byte(i)];

        if (byte == 0xFFu) {
            run = 0;
            continue;
        }

        const size_t limit = (total - i < BITMAP_BITS) ? total : i + BITMAP_BITS;

        for (size_t bit = 0; i < limit; i++, bit++) {
            if ((byte >> bit) & 1u) {
                run = 0;
                continue;
            }

            if (run == 0) {
                start = i;
            }
            if (++run == n) {
                for (size_t j = start; j < start + n; j++) {
                    bitmap_set_used((uint8_t *)bitmap, j);
                }
                palloc_state.free_pages -= n;
                spinlock_unlock(&palloc_state.lock);
                return base + (start << PAGE_SHIFT);
            }
        }
    }

    spinlock_unlock(&palloc_state.lock);
    return 0;
}

palloc_error_t pfree(uintptr_t addr) {
    return pfree_contiguous(addr, 1);
}

palloc_error_t pfree_contiguous(uintptr_t addr, size_t n) {
    if (unlikely(!palloc_state.initialized)) {
        return PALLOC_ERR_NOT_INITIALIZED;
    }

    if (unlikely(!is_page_aligned(addr))) {
        return PALLOC_ERR_NOT_ALIGNED;
    }

    if (n == 0) {
        return PALLOC_OK;
    }

    if (unlikely(!is_in_range(addr))) {
        return PALLOC_ERR_INVALID_ADDR;
    }

    const size_t start_page = addr_to_page(addr);
    const size_t end_page   = start_page + n;

    if (unlikely(end_page < start_page || end_page > palloc_state.total_pages)) {
        return PALLOC_ERR_INVALID_ADDR;
    }

    spinlock_lock(&palloc_state.lock);

    uint8_t *const bitmap = palloc_state.bitmap;
    size_t i = start_page;

    /* Pass 1: verify every page in range is currently used. */
    while (i < end_page) {
        const size_t byte_idx = page_to_byte(i);
        size_t next;
        const uint8_t mask = byte_range_mask(i, end_page, byte_idx, &next);
        if (unlikely((bitmap[byte_idx] & mask) != mask)) {
            spinlock_unlock(&palloc_state.lock);
            return PALLOC_ERR_ALREADY_FREE;
        }
        i = next;
    }

    /* Pass 2: clear bits and count. */
    size_t freed = 0;
    for (i = start_page; i < end_page; ) {
        const size_t byte_idx = page_to_byte(i);
        size_t next;
        const uint8_t mask  = byte_range_mask(i, end_page, byte_idx, &next);
        const uint8_t was   = bitmap[byte_idx];
        bitmap[byte_idx]    = (uint8_t)(was & ~mask);
        freed              += (size_t)__builtin_popcount(was & mask);
        i = next;
    }

    palloc_state.free_pages += freed;
    spinlock_unlock(&palloc_state.lock);
    return PALLOC_OK;
}

bool palloc_is_free(uintptr_t addr) {
    if (unlikely(!palloc_state.initialized || !is_page_aligned(addr) ||
                 !is_in_range(addr))) {
        return false;
    }

    spinlock_lock(&palloc_state.lock);
    bool free = !bitmap_test(palloc_state.bitmap, addr_to_page(addr));
    spinlock_unlock(&palloc_state.lock);
    return free;
}

bool palloc_is_initialized(void) {
    return palloc_state.initialized;
}

size_t palloc_free_count(void) {
    if (unlikely(!palloc_state.initialized)) {
        return 0;
    }

    spinlock_lock(&palloc_state.lock);
    size_t count = palloc_state.free_pages;
    spinlock_unlock(&palloc_state.lock);
    return count;
}

size_t palloc_total_count(void) {
    if (unlikely(!palloc_state.initialized)) {
        return 0;
    }
    return palloc_state.total_pages;
}

palloc_error_t palloc_get_stats(size_t *total, size_t *free_out) {
    if (unlikely(!palloc_state.initialized)) {
        return PALLOC_ERR_NOT_INITIALIZED;
    }

    if (unlikely(total == NULL || free_out == NULL)) {
        return PALLOC_ERR_INVALID_PARAM;
    }

    spinlock_lock(&palloc_state.lock);
    *total    = palloc_state.total_pages;
    *free_out = palloc_state.free_pages;
    spinlock_unlock(&palloc_state.lock);

    return PALLOC_OK;
}

const char *palloc_strerror(palloc_error_t err) {
    switch (err) {
        case PALLOC_OK:                return "ok";
        case PALLOC_ERR_NOMEM:         return "not enough memory";
        case PALLOC_ERR_NOT_INITIALIZED: return "not initialized";
        case PALLOC_ERR_INVALID_ADDR:  return "invalid address";
        case PALLOC_ERR_NOT_ALIGNED:   return "not page-aligned";
        case PALLOC_ERR_ALREADY_FREE:  return "page already free";
        case PALLOC_ERR_NOT_FREE:      return "page not free";
        case PALLOC_ERR_INVALID_PARAM: return "invalid parameter";
    }
    return NULL;
}
