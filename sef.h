#ifndef SCORPION_SEF_H
#define SCORPION_SEF_H

#include <stdint.h>

#define SEF_MAGIC      0x00464553
#define SEF_MAX_SEGMENTS 8

#define SEG_TEXT    0
#define SEG_DATA    1
#define SEG_BSS     2
#define SEG_RELOC   3
#define SEG_IMPORT  4
#define SEG_EXPORT  5

#define SEF_FLAG_PRIV_CONTROLLER 0x0001
#define SEF_FLAG_DYNAMIC         0x0002

/* SEF_R_* relocation record types (see docs/dynamic-linking.md) */
#define SEF_R_RELATIVE 0  /* add base to 32-bit word at offset */
#define SEF_R_HI20     1  /* add base to lui immediate  */
#define SEF_R_LO12I    2  /* add base to addi/lw immediate */
#define SEF_R_LO12S    3  /* add base to store immediate */
#define SEF_R_CALL     4  /* patch auipc+jalr pair at offset to absolute target */

#define SEF_MAX_LIBS   4

typedef struct {
    uint32_t type;
    uint32_t vaddr;
    uint32_t size;
    uint32_t offset;
} SefSegment;

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t value;
} SefRelocEntry;

typedef struct {
    uint32_t type;
    uint32_t slot;
    uint32_t name_len;
} SefImportRecord;

typedef struct {
    uint32_t value;
    uint32_t name_len;
} SefExportRecord;

typedef struct {
    uint32_t magic;
    uint32_t entry;
    uint16_t num_segments;
    uint16_t flags;
    SefSegment segments[];
} SefHeader;

#endif
