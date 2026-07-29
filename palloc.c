#include "scorpion.h"
#include <string.h>

#define BITMAP_BITS 8

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
    return (size_t)((addr - palloc_state.base) / PAGE_SIZE);
}

static inline size_t page_to_byte(size_t page) {
    return page / BITMAP_BITS;
}

static inline uint8_t page_to_bit(size_t page) {
    return (uint8_t)(page % BITMAP_BITS);
}

static inline bool bitmap_test(size_t page) {
    return (palloc_state.bitmap[page_to_byte(page)] >> page_to_bit(page)) & 1;
}

static inline void bitmap_set_used(size_t page) {
    palloc_state.bitmap[page_to_byte(page)] |= ((uint8_t)1 << page_to_bit(page));
}

static inline void bitmap_set_free(size_t page) {
    palloc_state.bitmap[page_to_byte(page)] &= ~((uint8_t)1 << page_to_bit(page));
}

palloc_error_t palloc_init(uintptr_t base, size_t size,
                           uint8_t *bitmap, size_t bitmap_size) {
    if (!is_page_aligned(base)) {
        return PALLOC_ERR_NOT_ALIGNED;
    }

    if (size < PAGE_SIZE) {
        return PALLOC_ERR_NOMEM;
    }

    if (bitmap == NULL || bitmap_size == 0) {
        return PALLOC_ERR_INVALID_PARAM;
    }

    size = (size / PAGE_SIZE) * PAGE_SIZE;
    size_t total = size / PAGE_SIZE;

    size_t required = (total + BITMAP_BITS - 1) / BITMAP_BITS;
    if (bitmap_size < required) {
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
    if (!palloc_state.initialized) {
        return PALLOC_ERR_NOT_INITIALIZED;
    }

    if (!is_page_aligned(start)) {
        return PALLOC_ERR_NOT_ALIGNED;
    }

    if (count == 0) {
        return PALLOC_OK;
    }

    if (start + count * PAGE_SIZE < start) {
        return PALLOC_ERR_INVALID_ADDR;
    }

    size_t start_page = addr_to_page(start);
    size_t end_page   = start_page + count;

    if (end_page > palloc_state.total_pages || !is_in_range(start)) {
        return PALLOC_ERR_INVALID_ADDR;
    }

    spinlock_lock(&palloc_state.lock);

    for (size_t i = start_page; i < end_page; i++) {
        if (!bitmap_test(i)) {
            spinlock_unlock(&palloc_state.lock);
            return PALLOC_ERR_ALREADY_FREE;
        }
        bitmap_set_free(i);
        palloc_state.free_pages++;
    }

    spinlock_unlock(&palloc_state.lock);
    return PALLOC_OK;
}

uintptr_t palloc(void) {
    return palloc_contiguous(1);
}

uintptr_t palloc_contiguous(size_t n) {
    if (!palloc_state.initialized || n == 0) {
        return 0;
    }

    spinlock_lock(&palloc_state.lock);

    if (palloc_state.free_pages < n) {
        spinlock_unlock(&palloc_state.lock);
        return 0;
    }

    size_t run  = 0;
    size_t start = 0;

    for (size_t i = 0; i < palloc_state.total_pages; i++) {
        if (bitmap_test(i)) {
            run = 0;
            continue;
        }

        if (run == 0) {
            start = i;
        }
        run++;

        if (run == n) {
            for (size_t j = start; j < start + n; j++) {
                bitmap_set_used(j);
            }
            palloc_state.free_pages -= n;
            spinlock_unlock(&palloc_state.lock);
            return palloc_state.base + start * PAGE_SIZE;
        }
    }

    spinlock_unlock(&palloc_state.lock);
    return 0;
}

palloc_error_t pfree(uintptr_t addr) {
    return pfree_contiguous(addr, 1);
}

palloc_error_t pfree_contiguous(uintptr_t addr, size_t n) {
    if (!palloc_state.initialized) {
        return PALLOC_ERR_NOT_INITIALIZED;
    }

    if (!is_page_aligned(addr)) {
        return PALLOC_ERR_NOT_ALIGNED;
    }

    if (n == 0) {
        return PALLOC_OK;
    }

    if (!is_in_range(addr)) {
        return PALLOC_ERR_INVALID_ADDR;
    }

    size_t start_page = addr_to_page(addr);
    size_t end_page   = start_page + n;

    if (end_page < start_page || end_page > palloc_state.total_pages) {
        return PALLOC_ERR_INVALID_ADDR;
    }

    spinlock_lock(&palloc_state.lock);

    for (size_t i = start_page; i < end_page; i++) {
        if (!bitmap_test(i)) {
            spinlock_unlock(&palloc_state.lock);
            return PALLOC_ERR_ALREADY_FREE;
        }
    }

    for (size_t i = start_page; i < end_page; i++) {
        bitmap_set_free(i);
    }
    palloc_state.free_pages += n;

    spinlock_unlock(&palloc_state.lock);
    return PALLOC_OK;
}

bool palloc_is_free(uintptr_t addr) {
    if (!palloc_state.initialized || !is_page_aligned(addr) ||
        !is_in_range(addr)) {
        return false;
    }

    size_t page = addr_to_page(addr);
    spinlock_lock(&palloc_state.lock);
    bool free = !bitmap_test(page);
    spinlock_unlock(&palloc_state.lock);
    return free;
}

bool palloc_is_initialized(void) {
    return palloc_state.initialized;
}

size_t palloc_free_count(void) {
    if (!palloc_state.initialized) {
        return 0;
    }

    spinlock_lock(&palloc_state.lock);
    size_t count = palloc_state.free_pages;
    spinlock_unlock(&palloc_state.lock);
    return count;
}

size_t palloc_total_count(void) {
    if (!palloc_state.initialized) {
        return 0;
    }
    return palloc_state.total_pages;
}

palloc_error_t palloc_get_stats(size_t *total, size_t *free_out) {
    if (!palloc_state.initialized) {
        return PALLOC_ERR_NOT_INITIALIZED;
    }

    if (total == NULL || free_out == NULL) {
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
