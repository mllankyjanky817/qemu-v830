#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/bitops.h"
#include "qemu/main-loop.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "hw/ssi/ssi.h"
#include "hw/v830/v832_soc.h"

#define V832_TEST_IO_BASE 0xc0001000u
#define V832_TEST_IO_SIZE 0x100
#define V832_TEST_EXTERNAL_IO_BASE 0x30000000u
#define V832_TEST_EXTERNAL_IO_SIZE 0x1000
#define V832_TEST_RAM_MAX_SIZE (32 * MiB)
#define V832_TEST_EXTERNAL_SIZE (128 * MiB)
#define V832_TEST_ROM_BASE 0x07000000u
#define V832_TEST_ROM_SIZE (16 * MiB)
#define V832_TEST_BLOCK_SIZE (V832_TEST_EXTERNAL_SIZE / 8)
#define V832_TEST_IMAGE_COUNT \
    (UINT32_MAX / V832_TEST_BLOCK_SIZE + 1)

#define TEST_INT_BASE 0x00
#define TEST_PORTB 0x10
#define TEST_PORTA 0x20
#define TEST_DMARQ 0x30
#define TEST_NMI 0x38
#define TEST_PORT_OUT 0x40
#define TEST_PORTA_OUT 0x44
#define TEST_PORTB_OUT 0x48
#define TEST_DMAAK 0x4c
#define TEST_TC_STOPAK 0x50
#define TEST_TC_STOPAK_COUNT 0x51
#define TEST_SSI_LAST 0x54
#define TEST_DMAAK_COUNT 0x58
#define TEST_NMI_DMA_ARM 0x59
#define TEST_NMI_STOPAK_ARM 0x5a
#define TEST_DMAAK_SEQUENCE 0x5c

#define TYPE_V832_TEST_SSI "v832-test-ssi"
#define TYPE_V832_TEST_BOARD MACHINE_TYPE_NAME("v832-test-board")

OBJECT_DECLARE_SIMPLE_TYPE(V832TestBoardState, V832_TEST_BOARD)

typedef struct V832TestSSILoopback {
    SSIPeripheral parent_obj;
    uint32_t last_value;
    V832TestBoardState *board;
} V832TestSSILoopback;

OBJECT_DECLARE_SIMPLE_TYPE(V832TestSSILoopback, V832_TEST_SSI)

struct V832TestBoardState {
    MachineState parent_obj;
    V832SoCState soc;
    Clock *osc_clk;
    MemoryRegion external;
    MemoryRegion external_alias[V832_TEST_IMAGE_COUNT];
    MemoryRegion rom;
    MemoryRegion test_io;
    MemoryRegion external_io;
    uint8_t intp;
    uint8_t portb;
    uint8_t porta;
    uint8_t dmarq;
    uint8_t nmi;
    uint8_t port_out;
    uint8_t porta_out;
    uint8_t portb_out;
    uint8_t dmaak;
    uint8_t dmaak_sequence[4];
    uint8_t dmaak_sequence_count;
    bool nmi_on_dmaak;
    uint8_t tc_stopak;
    uint32_t tc_stopak_count;
    bool nmi_on_stopak;
    QEMUBH *stopak_nmi_bh;
    uint32_t ssi_last;
    uint32_t external_io_value;
    qemu_irq intp_in[8];
    qemu_irq portb_in[8];
    qemu_irq porta_in[8];
    qemu_irq dmarq_in[4];
    qemu_irq nmi_in;
};

static uint32_t v832_test_ssi_transfer(SSIPeripheral *dev, uint32_t value)
{
    V832TestSSILoopback *loopback = V832_TEST_SSI(dev);

    loopback->last_value = value;
    if (loopback->board) {
        loopback->board->ssi_last = value;
    }
    return value;
}

static void v832_test_ssi_realize(SSIPeripheral *dev, Error **errp)
{
}

static void v832_test_ssi_class_init(ObjectClass *klass, const void *data)
{
    SSIPeripheralClass *ssc = SSI_PERIPHERAL_CLASS(klass);

    ssc->realize = v832_test_ssi_realize;
    ssc->transfer = v832_test_ssi_transfer;
    ssc->cs_polarity = SSI_CS_NONE;
}

static const TypeInfo v832_test_ssi_type = {
    .name = TYPE_V832_TEST_SSI,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(V832TestSSILoopback),
    .class_init = v832_test_ssi_class_init,
};

static void v832_test_set_input(qemu_irq *inputs, unsigned count,
                                uint8_t *state, int index, int level)
{
    if (index < 0 || (unsigned)index >= count) {
        return;
    }
    if (level) {
        *state |= BIT(index);
    } else {
        *state &= ~BIT(index);
    }
    qemu_set_irq(inputs[index], level);
}

static void v832_test_port_out(void *opaque, int index, int level)
{
    V832TestBoardState *s = opaque;

    if (level) {
        s->port_out |= BIT(index);
    } else {
        s->port_out &= ~BIT(index);
    }
}

static void v832_test_porta_out(void *opaque, int index, int level)
{
    V832TestBoardState *s = opaque;

    if (level) {
        s->porta_out |= BIT(index);
    } else {
        s->porta_out &= ~BIT(index);
    }
}

static void v832_test_portb_out(void *opaque, int index, int level)
{
    V832TestBoardState *s = opaque;

    if (level) {
        s->portb_out |= BIT(index);
    } else {
        s->portb_out &= ~BIT(index);
    }
}

static void v832_test_dmaak(void *opaque, int index, int level)
{
    V832TestBoardState *s = opaque;

    if (level) {
        s->dmaak |= BIT(index);
    } else {
        s->dmaak &= ~BIT(index);
        if (s->dmaak_sequence_count < ARRAY_SIZE(s->dmaak_sequence)) {
            s->dmaak_sequence[s->dmaak_sequence_count++] = index;
        }
    }
    if (!level && s->nmi_on_dmaak) {
        s->nmi_on_dmaak = false;
        s->nmi = 1;
        qemu_set_irq(s->nmi_in, 1);
        s->nmi = 0;
        qemu_set_irq(s->nmi_in, 0);
    }
}

static void v832_test_tc_stopak(void *opaque, int index, int level)
{
    V832TestBoardState *s = opaque;

    if (level) {
        s->tc_stopak = 1;
        s->tc_stopak_count++;
        if (s->nmi_on_stopak) {
            s->nmi_on_stopak = false;
            qemu_bh_schedule(s->stopak_nmi_bh);
        }
    } else {
        s->tc_stopak = 0;
    }
}

static void v832_test_stopak_nmi(void *opaque)
{
    V832TestBoardState *s = opaque;

    s->nmi = 1;
    qemu_set_irq(s->nmi_in, 1);
    s->nmi = 0;
    qemu_set_irq(s->nmi_in, 0);
}

static uint64_t v832_test_io_read(void *opaque, hwaddr offset,
                                  unsigned size)
{
    V832TestBoardState *s = opaque;

    switch (offset) {
    case TEST_INT_BASE: return s->intp;
    case TEST_PORTB: return s->portb;
    case TEST_PORTA: return s->porta;
    case TEST_DMARQ: return s->dmarq;
    case TEST_NMI: return s->nmi;
    case TEST_PORT_OUT: return s->port_out;
    case TEST_PORTA_OUT: return s->porta_out;
    case TEST_PORTB_OUT: return s->portb_out;
    case TEST_DMAAK: return s->dmaak;
    case TEST_TC_STOPAK: return s->tc_stopak;
    case TEST_TC_STOPAK_COUNT: return s->tc_stopak_count;
    case TEST_SSI_LAST: return s->ssi_last;
    case TEST_DMAAK_COUNT: return s->dmaak_sequence_count;
    default:
        if (offset >= TEST_DMAAK_SEQUENCE &&
            offset < TEST_DMAAK_SEQUENCE + ARRAY_SIZE(s->dmaak_sequence)) {
            return s->dmaak_sequence[offset - TEST_DMAAK_SEQUENCE];
        }
        break;
    }
    if (offset < TEST_INT_BASE + 8) {
        return (s->intp >> (offset - TEST_INT_BASE)) & 1;
    }
    return 0;
}

static void v832_test_io_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    V832TestBoardState *s = opaque;
    unsigned index;

    if (offset < TEST_INT_BASE + 8) {
        index = offset - TEST_INT_BASE;
        v832_test_set_input(s->intp_in, 8, &s->intp, index, value & 1);
    } else if (offset >= TEST_PORTB && offset < TEST_PORTB + 8) {
        index = offset - TEST_PORTB;
        v832_test_set_input(s->portb_in, 8, &s->portb, index, value & 1);
    } else if (offset >= TEST_PORTA && offset < TEST_PORTA + 8) {
        index = offset - TEST_PORTA;
        v832_test_set_input(s->porta_in, 8, &s->porta, index, value & 1);
    } else if (offset >= TEST_DMARQ && offset < TEST_DMARQ + 4) {
        index = offset - TEST_DMARQ;
        v832_test_set_input(s->dmarq_in, 4, &s->dmarq, index, value & 1);
    } else if (offset == TEST_NMI) {
        s->nmi = value & 1;
        qemu_set_irq(s->nmi_in, s->nmi);
    } else if (offset == TEST_DMAAK_COUNT) {
        s->dmaak_sequence_count = 0;
    } else if (offset == TEST_NMI_DMA_ARM) {
        s->nmi_on_dmaak = value & 1;
    } else if (offset == TEST_NMI_STOPAK_ARM) {
        s->nmi_on_stopak = value & 1;
    } else if (offset == TEST_TC_STOPAK_COUNT) {
        s->tc_stopak_count = 0;
    }
}

static const MemoryRegionOps v832_test_io_ops = {
    .read = v832_test_io_read,
    .write = v832_test_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t v832_test_external_io_read(void *opaque, hwaddr offset,
                                           unsigned size)
{
    V832TestBoardState *s = opaque;

    return s->external_io_value;
}

static void v832_test_external_io_write(void *opaque, hwaddr offset,
                                        uint64_t value, unsigned size)
{
    V832TestBoardState *s = opaque;

    if (size == 4) {
        s->external_io_value = value;
    } else if (size == 2) {
        s->external_io_value = (s->external_io_value & ~0xffffu) |
                               (value & 0xffffu);
    } else if (size == 1) {
        s->external_io_value = (s->external_io_value & ~0xffu) |
                               (value & 0xffu);
    }
}

static const MemoryRegionOps v832_test_external_io_ops = {
    .read = v832_test_external_io_read,
    .write = v832_test_external_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void v832_test_board_init(MachineState *machine)
{
    V832TestBoardState *s = V832_TEST_BOARD(machine);
    MemoryRegion *sysmem = get_system_memory();

    s->stopak_nmi_bh = qemu_bh_new(v832_test_stopak_nmi, s);

    if (machine->ram_size == 0 || machine->ram_size >= V832_TEST_RAM_MAX_SIZE) {
        error_report("V832 test board RAM must be less than 32 MiB");
        exit(EXIT_FAILURE);
    }

    memory_region_init(&s->external, OBJECT(machine), "v832-test-external",
                       V832_TEST_EXTERNAL_SIZE);
    memory_region_add_subregion(&s->external, 0, machine->ram);
    memory_region_init_ram(&s->rom, NULL, "v832-test-rom",
                           V832_TEST_ROM_SIZE, &error_fatal);
    if (machine->firmware) {
        gchar *image_data;
        gsize image_size;
        GError *err = NULL;

        if (!g_file_get_contents(machine->firmware, &image_data, &image_size,
                                 &err) || image_size == 0 ||
            image_size > V832_TEST_ROM_SIZE) {
            if (err) {
                error_report("could not load V832 test-board firmware: %s",
                             err->message);
                g_error_free(err);
            } else {
                error_report("invalid V832 test-board firmware size");
            }
            exit(EXIT_FAILURE);
        }
        for (size_t offset = 0; offset < V832_TEST_ROM_SIZE;
             offset += image_size) {
            size_t copy_size = MIN(image_size, V832_TEST_ROM_SIZE - offset);

            memcpy(memory_region_get_ram_ptr(&s->rom) + offset,
               image_data, copy_size);
        }
        g_free(image_data);
    }
    memory_region_set_readonly(&s->rom, true);
    memory_region_add_subregion(&s->external, V832_TEST_ROM_BASE, &s->rom);
    for (unsigned i = 8; i < V832_TEST_IMAGE_COUNT; i++) {
        memory_region_init_alias(&s->external_alias[i], OBJECT(machine),
                                 "v832-test-external-high-alias",
                                 &s->external,
                                 (i % 8) * V832_TEST_BLOCK_SIZE,
                                 V832_TEST_BLOCK_SIZE);
        memory_region_add_subregion(sysmem,
                                    (hwaddr)i * V832_TEST_BLOCK_SIZE,
                                    &s->external_alias[i]);
    }

    s->osc_clk = clock_new(OBJECT(machine), "osc");
    clock_set_hz(s->osc_clk, 23800000);
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_V832_SOC);
    s->soc.external = &s->external;
    object_property_set_bool(OBJECT(&s->soc.peripherals), "uart-loopback",
                             true, &error_fatal);
    qdev_connect_clock_in(DEVICE(&s->soc), "osc", s->osc_clk);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal)) {
        return;
    }

    memory_region_init_io(&s->test_io, OBJECT(machine), &v832_test_io_ops, s,
                          "v832-test-fixture", V832_TEST_IO_SIZE);
    memory_region_add_subregion(sysmem,
                                V830_IO_PHYS_BASE + 0x1000,
                                &s->test_io);
    memory_region_init_io(&s->external_io, OBJECT(machine),
                          &v832_test_external_io_ops, s,
                          "v832-test-external-io",
                          V832_TEST_EXTERNAL_IO_SIZE);
    memory_region_add_subregion_overlap(sysmem, V832_TEST_EXTERNAL_IO_BASE,
                                        &s->external_io, 20);

    for (unsigned i = 0; i < 8; i++) {
        s->intp_in[i] = qdev_get_gpio_in_named(DEVICE(&s->soc.peripherals),
                                               "intp", i);
        s->portb_in[i] = qdev_get_gpio_in_named(DEVICE(&s->soc.peripherals),
                                                 "portb-in", i);
        s->porta_in[i] = qdev_get_gpio_in_named(DEVICE(&s->soc.peripherals),
                                                 "porta-in", i);
        if (i < 4) {
            s->dmarq_in[i] = qdev_get_gpio_in(DEVICE(&s->soc.dma), i);
        }
    }
    s->nmi_in = qdev_get_gpio_in_named(DEVICE(&s->soc), "nmi", 0);

    for (unsigned i = 0; i < 5; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->soc.peripherals), "port-out", i,
                                    qemu_allocate_irq(v832_test_port_out, s, i));
    }
    for (unsigned i = 0; i < 8; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->soc.peripherals), "porta-out", i,
                                    qemu_allocate_irq(v832_test_porta_out, s, i));
        qdev_connect_gpio_out_named(DEVICE(&s->soc.peripherals), "portb-out", i,
                                    qemu_allocate_irq(v832_test_portb_out, s, i));
    }
    for (unsigned i = 0; i < 4; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->soc.dma), "dmaak", i,
                                    qemu_allocate_irq(v832_test_dmaak, s, i));
    }
    qdev_connect_gpio_out_named(DEVICE(&s->soc.dma), "tc_stopak", 0,
                                qemu_allocate_irq(v832_test_tc_stopak, s, 0));

    {
        V832TestSSILoopback *loopback =
            V832_TEST_SSI(qdev_new(TYPE_V832_TEST_SSI));

        loopback->board = s;
        ssi_realize_and_unref(DEVICE(loopback), s->soc.peripherals.csi_bus,
                              &error_fatal);
    }
}

static void v832_test_board_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "NEC V832 deterministic peripheral test board";
    mc->init = v832_test_board_init;
    mc->default_cpu_type = "v832-v830-cpu";
    mc->default_ram_size = 16 * MiB;
    mc->default_ram_id = "v832-test-board.ram";
}

static const TypeInfo v832_test_board_type = {
    .name = TYPE_V832_TEST_BOARD,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(V832TestBoardState),
    .class_init = v832_test_board_class_init,
};

static void v832_test_board_register_types(void)
{
    type_register_static(&v832_test_ssi_type);
    type_register_static(&v832_test_board_type);
}

type_init(v832_test_board_register_types)
