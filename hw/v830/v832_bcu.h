#ifndef HW_V830_V832_BCU_H
#define HW_V830_V832_BCU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_V832_BCU "v832-bcu"
OBJECT_DECLARE_SIMPLE_TYPE(V832BCUState, V832_BCU)

#define V832_BLOCK_SIZE (16 * MiB)
#define V832_BLOCK_COUNT 8
#define V832_SDRAM_BLOCKS 2

typedef struct V832BCURegion {
    MemoryRegion iomem;
    struct V832BCUState *state;
    hwaddr offset;
} V832BCURegion;

typedef struct V832BCUSDRAMRegion {
    MemoryRegion iomem;
    struct V832BCUState *state;
    unsigned block;
} V832BCUSDRAMRegion;

struct V832BCUState {
    SysBusDevice parent_obj;

    /* MMIO interface */
    V832BCURegion region[10];
    V832BCUSDRAMRegion sdram_region[V832_SDRAM_BLOCKS];
    
    /* Memory mappings */
    MemoryRegion *external;
    AddressSpace external_as;
    MemoryRegion block_alias[V832_BLOCK_COUNT];

    /* Register State */
    uint8_t bctc;
    uint8_t dbc;
    uint16_t pwc0;
    uint16_t pwc1;
    uint16_t pic0;
    uint16_t pic1;
    uint16_t sdc;
    uint16_t sdm;
    uint16_t rfc;
    uint8_t prc;

    /* SDRAM State */
    bool sdram_mode_set;
    bool sdram_refresh_enabled;
    bool sdram_row_open[V832_SDRAM_BLOCKS];
    uint32_t sdram_open_row[V832_SDRAM_BLOCKS];
    uint64_t sdram_accesses;
};

#endif