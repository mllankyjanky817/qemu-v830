#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "chardev/char.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties-system.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "hw/v830/v832_soc.h"

static void v832_soc_nmi(void *opaque, int n, int level)
{
    V832SoCState *s = opaque;

    if (n != 0) {
        return;
    }
    qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&s->cpu), "nmi", 0), level);
    qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&s->dma), "nmi", 0), level);
}

static void v832_soc_init(Object *obj)
{
    V832SoCState *s = V832_SOC(obj);

    object_initialize_child(obj, "cpu", &s->cpu, "v832-v830-cpu");
    object_initialize_child(obj, "bcu", &s->bcu, TYPE_V832_BCU);
    object_initialize_child(obj, "peripherals", &s->peripherals,
                            TYPE_V832_PERIPHERALS);
    object_initialize_child(obj, "dma", &s->dma, TYPE_V832_DMA);
    qdev_init_gpio_in_named(DEVICE(obj), v832_soc_nmi, "nmi", 1);

    s->osc_clk = qdev_init_clock_in(DEVICE(obj), "osc", NULL, NULL, 0);
    s->cpu_clk = clock_new(obj, "cpuclk");
    s->bus_clk = clock_new(obj, "busclk");
}

static void v832_soc_realize(DeviceState *dev, Error **errp)
{
    V832SoCState *s = V832_SOC(dev);
    MemoryRegion *sysmem = get_system_memory();

    if (!clock_has_source(s->osc_clk) ||
        (s->cmode != 0 && s->cmode != 1)) {
        error_setg(errp, "V832 SoC requires an oscillator and valid CMODE");
        return;
    }

    clock_set_source(s->cpu_clk, s->osc_clk);
    clock_set_mul_div(s->cpu_clk, 1, s->cmode ? 8 : 6);
    clock_set_source(s->bus_clk, s->osc_clk);
    clock_set_mul_div(s->bus_clk, 1, 2);
    clock_propagate(s->osc_clk);

    s->bcu.external = s->external;

    memory_region_init_ram(&s->internal_data_ram, NULL,
                           "v832-internal-data-ram",
                           V832_SOC_INTERNAL_RAM_SIZE, &error_fatal);
    memory_region_add_subregion_overlap(sysmem,
                                        V832_SOC_INTERNAL_DATA_RAM_BASE,
                                        &s->internal_data_ram, 10);

    memory_region_init_ram(&s->internal_insn_ram, NULL,
                           "v832-internal-instruction-ram",
                           V832_SOC_INTERNAL_RAM_SIZE, &error_fatal);
    memory_region_add_subregion_overlap(sysmem,
                                        V832_SOC_INTERNAL_INSN_RAM_BASE,
                                        &s->internal_insn_ram, 10);

    qdev_realize(DEVICE(&s->cpu), NULL, errp);
    if (*errp) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->bcu), errp)) {
        return;
    }
    {
        static const hwaddr bcu_offsets[] = {
            0x10, 0x12, 0x14, 0x16, 0x100, 0x102,
            0x110, 0x112, 0x122, 0x124,
        };
        unsigned index;

        for (index = 0; index < ARRAY_SIZE(bcu_offsets); index++) {
            sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->bcu), index,
                                    V830_IO_PHYS_BASE + bcu_offsets[index],
                                    20);
        }
    }

    qdev_prop_set_chr(DEVICE(&s->peripherals), "chardev", serial_hd(0));
    qdev_connect_clock_in(DEVICE(&s->peripherals), "clk", s->bus_clk);
    s->peripherals.cpu = &s->cpu;
    s->peripherals.dma = &s->dma;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->peripherals), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->peripherals), 0, V830_IO_PHYS_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->peripherals), 0,
                       qdev_get_gpio_in(DEVICE(&s->cpu), 0));

    s->dma.cpu = &s->cpu;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dma), errp)) {
        return;
    }
    for (unsigned channel = 0; channel < V832_DMA_CHANNELS; channel++) {
        qdev_connect_gpio_out_named(DEVICE(&s->peripherals), "dmarq-out",
                                    channel,
                                    qdev_get_gpio_in(DEVICE(&s->dma), channel));
        qdev_connect_gpio_out_named(DEVICE(&s->dma), "dmaak", channel,
                                    qdev_get_gpio_in_named(
                                        DEVICE(&s->peripherals), "dmaak-in",
                                        channel));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 0,
                       qdev_get_gpio_in_named(DEVICE(&s->peripherals),
                                              "dma-irq", 0));
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->dma), 0,
                            V830_IO_PHYS_BASE + 0x30, 10);
}

static const Property v832_soc_properties[] = {
    DEFINE_PROP_UINT8("cmode", V832SoCState, cmode, 0),
};

static void v832_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = v832_soc_realize;
    device_class_set_props(dc, v832_soc_properties);
}

static const TypeInfo v832_soc_types[] = {
    {
        .name = TYPE_V832_SOC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(V832SoCState),
        .instance_init = v832_soc_init,
        .class_init = v832_soc_class_init,
    },
};

DEFINE_TYPES(v832_soc_types)
