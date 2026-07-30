#include "alloc.h"
#include "scorpion.h"
#include <stdint.h>

#define MAX_PROC 5
#define BUDDY_STATE_SPLIT  2

extern uint8_t __heap_start[];
extern uint8_t __heap_end[];

static uint8_t *heap;
static size_t heap_size;
static size_t heap_offset = 0;
static BlockHeader *free_blocks = NULL;
static spinlock_t allocator_lock = { .locked = false };
static BuddyPage *metadata;
static BuddyBlock *free_lists[MAX_ORDER + 1];
static spinlock_t buddy_lock = { .locked = false };

static size_t total_buddy_pages;
static unsigned max_buddy_order;
static BuddyPage metadataStorage[HEAP_SIZE / PAGE_SIZE];

static size_t order_size(const unsigned order) {
    return PAGE_SIZE << order;
}

static unsigned ceil_log2_size(const size_t value) {
    if (value <= 1) {
        return 0;
    }

    #if SIZE_MAX == UINT64_MAX
        return 64u - (unsigned)__builtin_clzll(value - 1);
    #elif SIZE_MAX == UINT32_MAX
        return 32u - (unsigned)__builtin_clz((unsigned)(value - 1));
    #else
    #   error "Unsupported size_t width"
    #endif
}

static void *bump_alloc(size_t size) {
    const size_t total = ALLOC_HEADER_SIZE + size;
    if (total < size) {
        return NULL;
    }

    const size_t aligned_offset = ALIGN_UP(heap_offset, alignof(max_align_t));

    if (aligned_offset > heap_size || total > heap_size - aligned_offset) {
        return NULL;
    }

    BlockHeader *header = (BlockHeader *)&heap[aligned_offset];
    header->size = size;
    header->next = NULL;
    heap_offset = aligned_offset + total;

    return (void *)((uint8_t *)header + ALLOC_HEADER_SIZE);
}

static void *list_alloc(size_t size) {
    spinlock_lock(&allocator_lock);

    size = ALIGN_UP(size, alignof(max_align_t));

    BlockHeader **prev = &free_blocks;
    while (*prev != NULL) {
        BlockHeader *block = *prev;
        if (block->size >= size) {
            size_t remaining = block->size - size;
            if (remaining >= ALLOC_HEADER_SIZE + MIN_ALLOC_SIZE) {
                uint8_t *data = (uint8_t *)block + ALLOC_HEADER_SIZE;
                BlockHeader *remainder = (BlockHeader *)(data + size);
                remainder->size = remaining - ALLOC_HEADER_SIZE;
                remainder->next = block->next;
                *prev = remainder;
            } else {
                *prev = block->next;
            }
            block->size = size;
            spinlock_unlock(&allocator_lock);
            return (void *)((uint8_t *)block + ALLOC_HEADER_SIZE);
        }
        prev = &block->next;
    }

    void *ptr = bump_alloc(size);
    spinlock_unlock(&allocator_lock);
    return ptr;
}

static void free_mem(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    BlockHeader *block =
        (BlockHeader *)((uint8_t *)ptr - ALLOC_HEADER_SIZE);

    if ((uint8_t *)block < heap ||
        (uint8_t *)block >= heap + heap_size) {
        return;
    }

    spinlock_lock(&allocator_lock);

    BlockHeader **prev = &free_blocks;
    while (*prev != NULL && *prev < block) {
        prev = &(*prev)->next;
    }
    block->next = *prev;
    *prev = block;

    prev = &free_blocks;
    while (*prev != NULL && (*prev)->next != NULL) {
        BlockHeader *cur = *prev;
        uint8_t *cur_end =
            (uint8_t *)cur + ALLOC_HEADER_SIZE + cur->size;
        if (cur_end == (uint8_t *)cur->next) {
            cur->size += ALLOC_HEADER_SIZE + cur->next->size;
            cur->next = cur->next->next;
        } else {
            prev = &cur->next;
        }
    }

    spinlock_unlock(&allocator_lock);
}

static inline size_t buddy_page_idx(void *ptr) {
    return (size_t)((uint8_t *)ptr - heap) / PAGE_SIZE;
}

static inline BuddyBlock *buddy_page_block(size_t page_idx) {
    return (BuddyBlock *)&heap[page_idx * PAGE_SIZE];
}

static inline size_t buddy_buddy_idx(size_t page_idx, unsigned order) {
    return page_idx ^ (1u << order);
}

static void buddy_list_remove(unsigned order, BuddyBlock *target) {
    BuddyBlock **pp = &free_lists[order];
    while (*pp != NULL) {
        if (*pp == target) {
            *pp = target->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void buddy_list_push(unsigned order, BuddyBlock *block) {
    block->next = free_lists[order];
    free_lists[order] = block;
}

static BuddyBlock *buddy_split(BuddyBlock *block, unsigned from_order,
                                unsigned to_order) {
    size_t page_idx = buddy_page_idx(block);

    while (from_order > to_order) {
        from_order--;
        size_t split_page = page_idx + (1u << from_order);
        metadata[split_page].order = from_order;
        metadata[split_page].state = BUDDY_STATE_FREE;
        buddy_list_push(from_order, buddy_page_block(split_page));
    }

    metadata[page_idx].order = to_order;
    metadata[page_idx].state = BUDDY_STATE_USED;
    return buddy_page_block(page_idx);
}

static void buddy_init(void) {
    spinlock_init(&buddy_lock);

    for (unsigned i = 0; i <= MAX_ORDER; i++) {
        free_lists[i] = NULL;
    }

    total_buddy_pages = heap_size / PAGE_SIZE;
    max_buddy_order = ceil_log2_size(total_buddy_pages);
    if (max_buddy_order > MAX_ORDER) {
        max_buddy_order = MAX_ORDER;
    }

    metadata = metadataStorage;

    for (size_t i = 0; i < total_buddy_pages; i++) {
        metadata[i].order = 0;
        metadata[i].state = BUDDY_STATE_USED;
    }

    size_t remaining = total_buddy_pages;
    size_t current = 0;

    for (unsigned order = max_buddy_order; remaining > 0; ) {
        size_t block_size = 1u << order;
        if (remaining >= block_size && (current % block_size == 0)) {
            metadata[current].order = order;
            metadata[current].state = BUDDY_STATE_FREE;
            buddy_list_push(order, buddy_page_block(current));
            current += block_size;
            remaining -= block_size;
        } else {
            if (order == 0) {
                break;
            }
            order--;
        }
    }
}

extern uint8_t _user_arena_start[];
extern uint8_t _user_arena_end[];

static uint8_t *user_base;
static size_t user_size;
static size_t user_offset;
static BlockHeader *user_free;
static spinlock_t user_lock = { .locked = false };

void user_arena_init(void)
{
    user_base = _user_arena_start;
    user_size = (size_t)(_user_arena_end - _user_arena_start);
    user_offset = 0;
    user_free = NULL;
    spinlock_init(&user_lock);
}

void *ualloc_(size_t size)
{
    if (size == 0) return NULL;

    size = ALIGN_UP(size, alignof(max_align_t));

    spinlock_lock(&user_lock);

    BlockHeader **prev = &user_free;
    while (*prev) {
        BlockHeader *block = *prev;
        if (block->size >= size) {
            size_t remaining = block->size - size;
            if (remaining >= ALLOC_HEADER_SIZE + MIN_ALLOC_SIZE) {
                uint8_t *data = (uint8_t *)block + ALLOC_HEADER_SIZE;
                BlockHeader *rem = (BlockHeader *)(data + size);
                rem->size = remaining - ALLOC_HEADER_SIZE;
                rem->next = block->next;
                *prev = rem;
            } else {
                *prev = block->next;
            }
            block->size = size;
            spinlock_unlock(&user_lock);
            return (void *)((uint8_t *)block + ALLOC_HEADER_SIZE);
        }
        prev = &block->next;
    }

    if (user_offset + size > user_size) {
        spinlock_unlock(&user_lock);
        return NULL;
    }

    BlockHeader *hdr = (BlockHeader *)(user_base + user_offset);
    hdr->size = size;
    hdr->next = NULL;
    user_offset += ALLOC_HEADER_SIZE + size;

    spinlock_unlock(&user_lock);
    return (void *)((uint8_t *)hdr + ALLOC_HEADER_SIZE);
}

void ufree_(void *ptr)
{
    if (ptr == NULL) return;
    if ((uint8_t *)ptr < user_base ||
        (uint8_t *)ptr >= user_base + user_size) return;

    BlockHeader *block = (BlockHeader *)((uint8_t *)ptr - ALLOC_HEADER_SIZE);

    spinlock_lock(&user_lock);

    BlockHeader **prev = &user_free;
    while (*prev && *prev < block) prev = &(*prev)->next;
    block->next = *prev;
    *prev = block;

    prev = &user_free;
    while (*prev && (*prev)->next) {
        BlockHeader *cur = *prev;
        uint8_t *end = (uint8_t *)cur + ALLOC_HEADER_SIZE + cur->size;
        if (end == (uint8_t *)cur->next) {
            cur->size += ALLOC_HEADER_SIZE + cur->next->size;
            cur->next = cur->next->next;
        } else {
            prev = &cur->next;
        }
    }

    spinlock_unlock(&user_lock);
}

void alloc_init(void) {
    heap = __heap_start;
    heap_size = (size_t)(__heap_end - __heap_start);
    spinlock_init(&allocator_lock);
    buddy_init();
}

static void *buddy_alloc(const size_t size) {
    if (size == 0 || metadata == NULL) {
        return NULL;
    }

    const size_t pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    const unsigned order = ceil_log2_size(pages_needed);

    if (order > max_buddy_order) {
        return NULL;
    }

    spinlock_lock(&buddy_lock);

    for (unsigned o = order; o <= max_buddy_order; o++) {
        if (free_lists[o] != NULL) {
            BuddyBlock *block = free_lists[o];
            free_lists[o] = block->next;

            block = buddy_split(block, o, order);

            spinlock_unlock(&buddy_lock);
            return (void *)block;
        }
    }

    spinlock_unlock(&buddy_lock);
    return NULL;
}

void *alloc_(const size_t size) {
    if (size == 0) {
        return NULL;
    }

    if (size >= PAGE_SIZE) {
        return buddy_alloc(size);
    }

    return list_alloc(size);
}

static void buddy_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    if ((uint8_t *)ptr < heap || (uint8_t *)ptr >= heap + heap_size) {
        return;
    }

    if ((uintptr_t)ptr % PAGE_SIZE != 0) {
        return;
    }

    spinlock_lock(&buddy_lock);

    size_t page_idx = buddy_page_idx(ptr);

    if (metadata[page_idx].state != BUDDY_STATE_USED) {
        spinlock_unlock(&buddy_lock);
        return;
    }

    unsigned order = metadata[page_idx].order;

    metadata[page_idx].state = BUDDY_STATE_FREE;

    while (order < max_buddy_order) {
        size_t buddy = buddy_buddy_idx(page_idx, order);

        if (buddy >= total_buddy_pages ||
            metadata[buddy].state != BUDDY_STATE_FREE ||
            metadata[buddy].order != order) {
            break;
        }

        buddy_list_remove(order, buddy_page_block(buddy));

        metadata[buddy].state = BUDDY_STATE_SPLIT;

        if (buddy < page_idx) {
            page_idx = buddy;
        }

        order++;
        metadata[page_idx].order = order;
    }

    metadata[page_idx].state = BUDDY_STATE_FREE;
    buddy_list_push(order, buddy_page_block(page_idx));

    spinlock_unlock(&buddy_lock);
}

void free_(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    if ((uint8_t *)ptr < heap || (uint8_t *)ptr >= heap + heap_size) {
        return;
    }

    if ((uintptr_t)ptr % PAGE_SIZE == 0 &&
        buddy_page_idx(ptr) < total_buddy_pages &&
        metadata != NULL &&
        metadata[buddy_page_idx(ptr)].state == BUDDY_STATE_USED) {
        buddy_free(ptr);
        return;
    }

    free_mem(ptr);
}