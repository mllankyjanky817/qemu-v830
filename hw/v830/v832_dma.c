#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/registerfields.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "target/v830/cpu.h"
#include "hw/v830/v832_dma.h"

#define V832_DMA_BASE 0xc0000030u
#define V832_DMA_INT_SOURCE 7

/* Channel Register Offsets */
#define REG_DSAH 0x00
#define REG_DSAL 0x02
#define REG_DDAH 0x04
#define REG_DDAL 0x06
#define REG_DBCH 0x08
#define REG_DBCL 0x0a
#define REG_DCHC 0x0c
#define REG_DC   0x3e

/* DCHC Register Fields */
FIELD(DCHC, EN,   0, 1)
FIELD(DCHC, DS,   1, 2)
FIELD(DCHC, TM,   3, 1)
FIELD(DCHC, DRL,  4, 1)
FIELD(DCHC, DAL,  5, 1)
FIELD(DCHC, DAD,  6, 2)
FIELD(DCHC, SAD,  8, 2)
FIELD(DCHC, TBT, 10, 2)
FIELD(DCHC, TTYP,12, 3)

/* DC Register Fields */
FIELD(DC, MEN,  0, 1)
FIELD(DC, TC,   4, 4)
FIELD(DC, TCSA, 8, 1)

static bool v832_dma_valid_control(uint16_t dchc)
{
    unsigned transfer_type = FIELD_EX16(dchc, DCHC, TTYP);
    unsigned transfer_block = FIELD_EX16(dchc, DCHC, TBT);
    unsigned source_dir = FIELD_EX16(dchc, DCHC, SAD);
    unsigned dest_dir = FIELD_EX16(dchc, DCHC, DAD);
    unsigned data_size = FIELD_EX16(dchc, DCHC, DS);

    return transfer_type != 2 && transfer_type != 3 &&
           transfer_block != 3 &&
           source_dir != 3 &&
           dest_dir != 3 &&
           data_size != 3;
}

static bool v832_dma_internal_address(hwaddr address)
{
    return address < 0x1000 ||
           (address >= 0xfe000000u && address < 0xfe001000u) ||
           (address >= 0xc0000000u && address < 0xc0000400u);
}

static unsigned v832_dma_width(const V832DMAChannel *channel)
{
    switch (FIELD_EX16(channel->dchc, DCHC, DS)) {
    case 0:  return 1;
    case 1:  return 2;
    case 2:  return 4;
    default: return 0;
    }
}

static void v832_dma_advance(uint32_t *address, unsigned direction,
                             unsigned width)
{
    switch (direction) {
    case 0:
        *address += width;
        break;
    case 1:
        *address -= width;
        break;
    case 2:
        break;
    default:
        g_assert_not_reached();
    }
}

static void v832_dma_set_ack(V832DMAState *s, unsigned index, bool active)
{
    V832DMAChannel *channel = &s->channel[index];
    bool dal = FIELD_EX16(channel->dchc, DCHC, DAL);

    /* Assert or deassert ACK based on configured active level */
    qemu_set_irq(s->dmaak[index], active ? (dal ? 1 : 0) : (dal ? 0 : 1));
}

static void v832_dma_raise_tc(V832DMAState *s)
{
    qemu_set_irq(s->tc_stopak, 1);
    qemu_set_irq(s->tc_stopak, 0);
}

static void v832_dma_raise_irq(V832DMAState *s)
{
    if (s->cpu) {
        v830_cpu_set_interrupt_source(s->cpu, V832_DMA_INT_SOURCE);
        cpu_interrupt(CPU(s->cpu), CPU_INTERRUPT_HARD);
    }
}

static bool v832_dma_transfer(V832DMAState *s, V832DMAChannel *channel)
{
    uint8_t data[4];
    unsigned width = v832_dma_width(channel);
    bool first_transfer = channel->dbc == 0;

    if (!width || !v832_dma_valid_control(channel->dchc) ||
        (!first_transfer && channel->dbc < width) ||
        v832_dma_internal_address(channel->dsa) ||
        v832_dma_internal_address(channel->dda)) {
        qemu_log_mask(LOG_GUEST_ERROR, "V832 DMA: invalid transfer configuration\n");
        return false;
    }

    if (address_space_read(&address_space_memory, channel->dsa,
                           MEMTXATTRS_UNSPECIFIED, data, width) != MEMTX_OK) {
        return false;
    }

    if (address_space_write(&address_space_memory, channel->dda,
                            MEMTXATTRS_UNSPECIFIED, data, width) != MEMTX_OK) {
        return false;
    }

    v832_dma_advance(&channel->dsa,
                     FIELD_EX16(channel->dchc, DCHC, SAD), width);
    v832_dma_advance(&channel->dda,
                     FIELD_EX16(channel->dchc, DCHC, DAD), width);
    if (!first_transfer) {
        channel->dbc -= width;
    }

    if (first_transfer || channel->dbc == 0) {
        unsigned index = channel - s->channel;

        channel->dchc = FIELD_DP16(channel->dchc, DCHC, EN, 0);
        s->dc |= BIT(R_DC_TC_SHIFT + index);

        v832_dma_set_ack(s, index, false);
        v832_dma_raise_tc(s);
        v832_dma_raise_irq(s);
    }
    return true;
}

static bool v832_dma_channel_requested(const V832DMAState *s,
                                       unsigned index)
{
    const V832DMAChannel *channel = &s->channel[index];
    unsigned transfer_type = FIELD_EX16(channel->dchc, DCHC, TTYP);

    if (!FIELD_EX16(channel->dchc, DCHC, EN) ||
        !v832_dma_valid_control(channel->dchc)) {
        return false;
    }

    switch (transfer_type) {
    case V832_DMA_REQUEST_EXTERNAL:
        return channel->request;
    case V832_DMA_REQUEST_SOFTWARE:
        return channel->software_request;
    default:
        return transfer_type < ARRAY_SIZE(s->pending_internal) &&
               s->pending_internal[transfer_type];
    }
}

static bool v832_dma_arbitrate(V832DMAState *s)
{
    bool transferred = false;

    if (!FIELD_EX16(s->dc, DC, MEN)) {
        return false;
    }

    for (;;) {
        unsigned index;
        V832DMAChannel *channel;
        unsigned transfer_type;

        for (index = 0; index < V832_DMA_CHANNELS; index++) {
            if (v832_dma_channel_requested(s, index)) {
                break;
            }
        }
        if (index == V832_DMA_CHANNELS) {
            break;
        }

        channel = &s->channel[index];
        transfer_type = FIELD_EX16(channel->dchc, DCHC, TTYP);
        v832_dma_set_ack(s, index, true);

        if (!v832_dma_transfer(s, channel)) {
            channel->dchc = FIELD_DP16(channel->dchc, DCHC, EN, 0);
        } else {
            transferred = true;
        }
        v832_dma_set_ack(s, index, false);

        if (transfer_type == V832_DMA_REQUEST_EXTERNAL) {
            if (!FIELD_EX16(channel->dchc, DCHC, TM) ||
                !channel->request) {
                channel->request = false;
            }
        } else if (transfer_type == V832_DMA_REQUEST_SOFTWARE) {
            channel->software_request = false;
        } else if (transfer_type < ARRAY_SIZE(s->pending_internal)) {
            s->pending_internal[transfer_type] = false;
        }
    }
    return transferred;
}

void v832_dma_set_internal_request(V832DMAState *s, enum V832DMARequest request)
{
    if (request >= ARRAY_SIZE(s->pending_internal)) {
        return;
    }
    s->pending_internal[request] = true;
    v832_dma_arbitrate(s);
}

static void v832_dma_request(void *opaque, int n, int level)
{
    V832DMAState *s = opaque;

    if (n < 0 || n >= V832_DMA_CHANNELS) {
        return;
    }

    V832DMAChannel *channel = &s->channel[n];
    bool active = FIELD_EX16(channel->dchc, DCHC, DRL) ? !level : level;
    channel->request = active;

    if (active) {
        v832_dma_arbitrate(s);
    }
}

static uint64_t v832_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    V832DMAState *s = opaque;
    unsigned index = offset / 0x10;
    unsigned reg = offset & 0x0f;

    if (offset == REG_DC && size == 2) {
        uint16_t value = s->dc;
        s->dc &= ~R_DC_TC_MASK;
        return value;
    }

    if (size != 2 || index >= V832_DMA_CHANNELS) {
        return 0;
    }

    V832DMAChannel *channel = &s->channel[index];
    switch (reg) {
    case REG_DSAH: return channel->dsa >> 16;
    case REG_DSAL: return channel->dsa & 0xffff;
    case REG_DDAH: return channel->dda >> 16;
    case REG_DDAL: return channel->dda & 0xffff;
    case REG_DBCH: return channel->dbc >> 16;
    case REG_DBCL: return channel->dbc & 0xffff;
    case REG_DCHC: return channel->dchc;
    default:       return 0;
    }
}

static void v832_dma_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    V832DMAState *s = opaque;
    unsigned index = offset / 0x10;
    unsigned reg = offset & 0x0f;

    if (size != 2) {
        return;
    }

    if (offset == REG_DC) {
        s->dc = (s->dc & R_DC_TC_MASK) | (value & (R_DC_TCSA_MASK | R_DC_MEN_MASK));
        v832_dma_arbitrate(s);
        return;
    }

    if (index >= V832_DMA_CHANNELS) {
        return;
    }

    V832DMAChannel *channel = &s->channel[index];
    switch (reg) {
    case REG_DSAH: channel->dsa = (channel->dsa & 0x0000ffff) | (value << 16); break;
    case REG_DSAL: channel->dsa = (channel->dsa & 0xffff0000) | (value & 0xffff); break;
    case REG_DDAH: channel->dda = (channel->dda & 0x0000ffff) | (value << 16); break;
    case REG_DDAL: channel->dda = (channel->dda & 0xffff0000) | (value & 0xffff); break;
    case REG_DBCH: channel->dbc = (channel->dbc & 0x0000ffff) | ((value & 0xff) << 16); break;
    case REG_DBCL: channel->dbc = (channel->dbc & 0xffff0000) | (value & 0xffff); break;
    case REG_DCHC:
        channel->dchc = value;
        channel->software_request =
            FIELD_EX16(channel->dchc, DCHC, TTYP) ==
            V832_DMA_REQUEST_SOFTWARE;
        v832_dma_arbitrate(s);
        break;
    }
}

static const MemoryRegionOps v832_dma_ops = {
    .read = v832_dma_read,
    .write = v832_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 2,
};

static void v832_dma_reset_hold(Object *obj, ResetType type)
{
    V832DMAState *s = V832_DMA(obj);

    memset(s->channel, 0, sizeof(s->channel));
    memset(s->pending_internal, 0, sizeof(s->pending_internal));
    s->dc = 0;

    for (unsigned i = 0; i < V832_DMA_CHANNELS; i++) {
        qemu_set_irq(s->dmaak[i], 1);
    }
    qemu_set_irq(s->tc_stopak, 0);
}

static void v832_dma_realize(DeviceState *dev, Error **errp)
{
    V832DMAState *s = V832_DMA(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &v832_dma_ops, s,
                          "v832-dma", V832_DMA_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    qdev_init_gpio_in(dev, v832_dma_request, V832_DMA_CHANNELS);
    qdev_init_gpio_out_named(dev, s->dmaak, "dmaak", V832_DMA_CHANNELS);
    qdev_init_gpio_out_named(dev, &s->tc_stopak, "tc_stopak", 1);
}

static void v832_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = v832_dma_realize;
    rc->phases.hold = v832_dma_reset_hold;
}

static const TypeInfo v832_dma_types[] = {
    {
        .name = TYPE_V832_DMA,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(V832DMAState),
        .class_init = v832_dma_class_init,
    },
};

DEFINE_TYPES(v832_dma_types)