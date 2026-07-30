#ifndef SCORPION_SEF_H
#define SCORPION_SEF_H

#include <stdint.h>

#define SEF_MAGIC      0x00464553
#define SEF_MAX_SEGMENTS 8

#define SEG_TEXT  0
#define SEG_DATA  1
#define SEG_BSS   2

#define SEF_FLAG_PRIV_CONTROLLER 0x0001

typedef struct {
    uint32_t type;
    uint32_t vaddr;
    uint32_t size;
    uint32_t offset;
} SefSegment;

typedef struct {
    uint32_t magic;
    uint32_t entry;
    uint16_t num_segments;
    uint16_t flags;
    SefSegment segments[];
} SefHeader;

#endif
