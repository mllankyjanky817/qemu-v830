//v832 Bus Control Unit (BCU) implementation.
// Can possibly be modified for supporting v831 and v833 in the future. The latter has additional registers I'll find in a ROM dump.
// That'll be found in a dump of the Pioneer AVIC D3's ROM since that has 64 MiB of SDRAM only connected to CS0, starting at address 0x48000000.
// This possibly means there is another BCU register responsible for mapping the SDRAM to which 128 MiB image in virtual memory it should reside.
// Also, since the AVIC D3 was equipped with two K4S561632H-UL75 SDRAM chips, and that the row address is 13 bits wide,
// SDC.RAW should be able to be set to accept 0b10 = 0x2 to support 13-bit row addresses, only for the v833.
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/bitops.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/v830/v832_bcu.h"

#define REG_BCTC 0x10
#define REG_DBC  0x12
#define REG_PWC0 0x14
#define REG_PWC1 0x16
#define REG_RFC  0x22
#define REG_PRC  0x24
#define REG_PIC0 0x100
#define REG_PIC1 0x102
#define REG_SDC  0x110
#define REG_SDM  0x112

static const hwaddr v832_bcu_reg_offsets[] = {
    REG_BCTC, REG_DBC, REG_PWC0, REG_PWC1, REG_RFC, REG_PRC, REG_PIC0,
    REG_PIC1, REG_SDC, REG_SDM 
};

static const unsigned v832_bcu_reg_sizes[] = {
    1, 1, 2, 2, 2, 1, 2, 2, 2, 2
};

static void v832_bcu_update_blocks(V832BCUState *s)
{
    memory_region_transaction_begin(); // reconfig 
    for (unsigned block = 2; block < V832_BLOCK_COUNT; block++) {
        bool enabled = !(block >= 3 && block <= 6 && (s->bctc & BIT(block))); // BCTC controls. Incomplete since this just enables/disables blocks 3-6 instead of 1 for I/O, 0 for ROM.
        memory_region_set_enabled(&s->block_alias[block], enabled); // also block 1 should be able to switch between ROM if 0, SDRAM if 1.
    }
    memory_region_transaction_commit();
}

static void v832_bcu_sdram_touch(V832BCUState *s, unsigned block, hwaddr address)
{
    unsigned column_bits = (s->sdc & BIT(8)) ? 9 : 8;
    uint32_t row = (address >> (column_bits + 2)) & (s->sdc & BIT(10) ? 0x0fff : 0x07ff); // SDC.RAW: 12 bits if 1, 11 bits if 0.

    s->sdram_accesses++;
    if (!s->sdram_row_open[block] || s->sdram_open_row[block] != row) {
        s->sdram_open_row[block] = row;
        s->sdram_row_open[block] = true;
    }
}

static MemTxResult v832_bcu_sdram_read(void *opaque, hwaddr addr,
                                       uint64_t *data, unsigned size,
                                       MemTxAttrs attrs)
{
    V832BCUSDRAMRegion *region = opaque;

    if (region->block == 0 || ((region->state->bctc & BIT(1)) && region->block == 1)) { // check if we're in an SDRAM region.
        v832_bcu_sdram_touch(region->state, region->block, addr);
    }
    return address_space_read(&region->state->external_as,
                              region->block * V832_BLOCK_SIZE + addr,
                              MEMTXATTRS_UNSPECIFIED, data, size);
}

static MemTxResult v832_bcu_sdram_write(void *opaque, hwaddr addr,
                                        uint64_t data, unsigned size,
                                        MemTxAttrs attrs)
{
    V832BCUSDRAMRegion *region = opaque;

    if (region->block == 0 || ((region->state->bctc & BIT(1)) && region->block == 1)) { // ditto
        v832_bcu_sdram_touch(region->state, region->block, addr);
    }
    return address_space_write(&region->state->external_as,
                               region->block * V832_BLOCK_SIZE + addr,
                               MEMTXATTRS_UNSPECIFIED, &data, size);
}

static const MemoryRegionOps v832_bcu_sdram_ops = { // define the methods for sdram operations.
    .read_with_attrs = v832_bcu_sdram_read,
    .write_with_attrs = v832_bcu_sdram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t v832_bcu_read(void *opaque, hwaddr offset, unsigned size) // return the value of the register at the given offset. Why uin64_t return?
{
    V832BCURegion *region = opaque;
    V832BCUState *s = region->state;

    switch (region->offset) {
    case REG_BCTC: return s->bctc;
    case REG_DBC:  return s->dbc;
    case REG_PWC0: return s->pwc0;
    case REG_PWC1: return s->pwc1;
    case REG_PIC0: return s->pic0;
    case REG_PIC1: return s->pic1;
    case REG_SDC:  return s->sdc;
    case REG_RFC:  return s->rfc;
    case REG_PRC:  return s->prc;
    case REG_SDM:
    default:       return 0;
    }
}

static void v832_bcu_write(void *opaque, hwaddr offset, // write to bcu registers.
                           uint64_t value, unsigned size)
{
    V832BCURegion *region = opaque;
    V832BCUState *s = region->state;

    switch (region->offset) {
    case REG_BCTC:
        s->bctc = value & 0xfa;
        v832_bcu_update_blocks(s);
        break;
    case REG_DBC:  s->dbc = value; break;
    case REG_PWC0: s->pwc0 = value; break;
    case REG_PWC1: s->pwc1 = value; break;
    case REG_PIC0: s->pic0 = value; break;
    case REG_PIC1: s->pic1 = value; break;
    case REG_SDC:  s->sdc = value & 0x9f1f; break;
    case REG_SDM:
        s->sdm = value;
        s->sdram_mode_set = true;
        break;
    case REG_RFC:
        s->rfc = value & 0x836f;
        s->sdram_refresh_enabled = !!(s->rfc & BIT(15));
        break;
    case REG_PRC:  s->prc = value & 0x07; break;
    default:       break;
    }
}

static const MemoryRegionOps v832_bcu_ops = { // a layer of abstraction from sdram ops.
    .read = v832_bcu_read,
    .write = v832_bcu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static void v832_bcu_reset_hold(Object *obj, ResetType type) // initial values upon reset.
{
    V832BCUState *s = V832_BCU(obj);

    s->bctc = 0;
    s->dbc = 0;
    s->pwc0 = 0x7770;
    s->pwc1 = 0x7fff;
    s->pic0 = 0x7777;
    s->pic1 = 0x7777;
    s->sdc = 0x001f;
    s->sdm = 0;
    s->rfc = 0x8000;
    s->prc = 0x07;
    s->sdram_mode_set = false;
    s->sdram_refresh_enabled = true;
    s->sdram_accesses = 0;

    for (int i = 0; i < V832_SDRAM_BLOCKS; i++) { // init sdram state.
        s->sdram_row_open[i] = false;
        s->sdram_open_row[i] = 0;
    }

    v832_bcu_update_blocks(s);
}

static void v832_bcu_realize(DeviceState *dev, Error **errp)
{
    V832BCUState *s = V832_BCU(dev);

    if (!s->external) {
        //error_setg(errp, "V832 BCU requires an external memory region");
        // we don't necessarily need external memory; all cpus in the v830 family have a 4 KiB internal region of data ram, and another 4 KiB for instruction ram.
        return;
    }
    address_space_init(&s->external_as, s->external, "v832-bcu-external"); // otherwise, realize the external space.

    for (unsigned i = 0; i < ARRAY_SIZE(v832_bcu_reg_offsets); i++) { // realize bcu registers.
        s->region[i].state = s;
        s->region[i].offset = v832_bcu_reg_offsets[i];
        memory_region_init_io(&s->region[i].iomem, OBJECT(dev),
                              &v832_bcu_ops, &s->region[i],
                              "v832-bcu-reg", v832_bcu_reg_sizes[i]);
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->region[i].iomem);
    }

    for (unsigned i = 0; i < V832_BLOCK_COUNT; i++) {
        if (i < V832_SDRAM_BLOCKS) {
            s->sdram_region[i].state = s;
            s->sdram_region[i].block = i;
            memory_region_init_io(&s->sdram_region[i].iomem, OBJECT(dev),
                                  &v832_bcu_sdram_ops, &s->sdram_region[i],
                                  "v832-sdram", V832_BLOCK_SIZE);
            // add the sdram region to the system memory.
            memory_region_add_subregion_overlap(get_system_memory(),
                                                i * V832_BLOCK_SIZE,
                                                &s->sdram_region[i].iomem, 5);
        } else { // start aliasing
            memory_region_init_alias(&s->block_alias[i], OBJECT(dev),
                                     "v832-bcu-block", s->external,
                                     i * V832_BLOCK_SIZE, V832_BLOCK_SIZE);
            memory_region_add_subregion_overlap(get_system_memory(),
                                                i * V832_BLOCK_SIZE,
                                                &s->block_alias[i], 5);
        }
    }
}

static void v832_bcu_class_init(ObjectClass *klass, const void *data) // init object
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = v832_bcu_realize;
    rc->phases.hold = v832_bcu_reset_hold;
}

static const TypeInfo v832_bcu_types[] = { // define the type info for the v832 bcu.
    {
        .name = TYPE_V832_BCU,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(V832BCUState),
        .class_init = v832_bcu_class_init,
    },
};

DEFINE_TYPES(v832_bcu_types)