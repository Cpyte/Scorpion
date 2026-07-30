#ifndef SCORPION_SEXEC_H
#define SCORPION_SEXEC_H

#include <stdint.h>

#define SEXEC_MAGIC      0x45584553
#define SEXEC_MAX_SEGMENTS 8

#define SEG_TEXT  0
#define SEG_DATA  1
#define SEG_BSS   2

#define SEXEC_FLAG_PRIV_CONTROLLER 0x0001

typedef struct {
    uint32_t type;
    uint32_t vaddr;
    uint32_t size;
    uint32_t offset;
} SexecSegment;

typedef struct {
    uint32_t magic;
    uint32_t entry;
    uint16_t num_segments;
    uint16_t flags;
    SexecSegment segments[];
} SexecHeader;

#endif
