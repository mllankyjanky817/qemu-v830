#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/clock.h"
#include "hw/core/sysbus.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "chardev/char.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/v830/v832_soc.h"

#define V832_RAM_MAX_SIZE (32 * MiB)
#define V831_V832_EXTERNAL_IMAGE_SIZE (128 * MiB)
#define V831_V832_V833_ROM_BASE 0x07000000
#define V831_V832_V833_ROM_SIZE (16 * MiB)
#define V831_V832_V833_IO_BASE 0xc0000000
#define V831_V832_V833_EXTERNAL_IO_TEST_BASE 0x30000000
#define V831_V832_V833_EXTERNAL_IO_TEST_SIZE 0x10
#define V831_V832_V833_EXTERNAL_BLOCK_SIZE (V831_V832_EXTERNAL_IMAGE_SIZE / 8)
#define V831_V832_EXTERNAL_IMAGE_COUNT (UINT32_MAX / V831_V832_V833_EXTERNAL_BLOCK_SIZE + 1)

typedef struct V832BoardState {
    MachineState parent_obj;
    V832SoCState soc;
    Clock *osc_clk;
    uint8_t cmode;
    MemoryRegion external;
    MemoryRegion external_alias[V831_V832_EXTERNAL_IMAGE_COUNT];
    MemoryRegion external_io;
    uint32_t external_io_value;
    MemoryRegion rom;
} V832BoardState;

#define TYPE_V832_BOARD MACHINE_TYPE_NAME("v832-board")
OBJECT_DECLARE_SIMPLE_TYPE(V832BoardState, V832_BOARD)

static uint64_t v832_external_io_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    V832BoardState *board = opaque;

    return board->external_io_value;
}

static void v832_external_io_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    V832BoardState *board = opaque;

    if (size == 4) {
        board->external_io_value = value;
    } else if (size == 2) {
        board->external_io_value = (board->external_io_value & ~0xffffu) |
                                   (value & 0xffffu);
    } else if (size == 1) {
        board->external_io_value = (board->external_io_value & ~0xffu) |
                                   (value & 0xffu);
    }
}

static const MemoryRegionOps v832_external_io_ops = {
    .read = v832_external_io_read,
    .write = v832_external_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void v832_board_get_cmode(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    V832BoardState *board = V832_BOARD(obj);
    uint8_t cmode = board->cmode;

    visit_type_uint8(v, name, &cmode, errp);
}

static void v832_board_set_cmode(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    V832BoardState *board = V832_BOARD(obj);
    uint8_t cmode;

    if (!visit_type_uint8(v, name, &cmode, errp)) {
        return;
    }
    if (cmode > 1) {
        error_setg(errp, "CMODE must be 0 or 1");
        return;
    }
    board->cmode = cmode;
}

static void v832_board_instance_init(Object *obj)
{
    V832BoardState *board = V832_BOARD(obj);

    board->cmode = 0; //Default CMODE is 0, which multiplies input clock frequency by 6
}

static void v832_board_init(MachineState *machine)
{
    V832BoardState *board = V832_BOARD(machine);
    MemoryRegion *sysmem = get_system_memory();
    unsigned i;
    if (board->cmode != 0 && board->cmode != 1) {
        error_report("V832 CMODE must be either 0 or 1");
        exit(EXIT_FAILURE);
    }

    if (machine->ram_size == 0 || machine->ram_size >= V832_RAM_MAX_SIZE) {
        error_report("V832 SDRAM size must be less than 32 MiB");
        exit(EXIT_FAILURE);
    }

    memory_region_init(&board->external, OBJECT(machine), "v832-external",
                       V831_V832_EXTERNAL_IMAGE_SIZE);
    memory_region_add_subregion(&board->external, 0, machine->ram);

    memory_region_init_ram(&board->rom, NULL, "v832.rom",
                           V831_V832_V833_ROM_SIZE, &error_fatal);
    if (machine->firmware) {
        gchar *image_data = NULL;
        gsize image_size;
        GError *error = NULL;

        if (!g_file_get_contents(machine->firmware, &image_data, &image_size,
                                 &error) || image_size == 0 ||
            image_size > V831_V832_V833_ROM_SIZE) {
            error_report("could not load V832 ROM '%s': %s",
                         machine->firmware,
                         error ? error->message : "invalid image size");
            g_clear_error(&error);
            g_free(image_data);
            exit(EXIT_FAILURE);
        }

        for (size_t offset = 0; offset < V831_V832_V833_ROM_SIZE; offset += image_size) {
            size_t copy_size = MIN(image_size, V831_V832_V833_ROM_SIZE - offset);

            memcpy(memory_region_get_ram_ptr(&board->rom) + offset,
                   image_data, copy_size);
        }
        g_free(image_data);
    }
    memory_region_set_readonly(&board->rom, true);
    memory_region_add_subregion(&board->external,
                                V831_V832_V833_ROM_BASE, &board->rom);

    for (i = 8; i < V831_V832_EXTERNAL_IMAGE_COUNT; i++) {
        memory_region_init_alias(&board->external_alias[i], OBJECT(machine),
                                 "v832-external-high-alias", &board->external,
                                 (i % 8) * V831_V832_V833_EXTERNAL_BLOCK_SIZE,
                                 V831_V832_V833_EXTERNAL_BLOCK_SIZE);
        memory_region_add_subregion(sysmem,
                                    (hwaddr)i * V831_V832_V833_EXTERNAL_BLOCK_SIZE,
                                    &board->external_alias[i]);
    }


    memory_region_init_io(&board->external_io, OBJECT(machine),
                          &v832_external_io_ops, board,
                          "v832-external-io-test", V831_V832_V833_EXTERNAL_IO_TEST_SIZE);
    memory_region_add_subregion_overlap(sysmem, V831_V832_V833_EXTERNAL_IO_TEST_BASE,
                                        &board->external_io, 20);

    board->osc_clk = clock_new(OBJECT(machine), "osc");

    clock_set_hz(board->osc_clk, 23800000);

    object_initialize_child(OBJECT(machine), "soc", &board->soc,
                            TYPE_V832_SOC);
    board->soc.external = &board->external;
    object_property_set_uint(OBJECT(&board->soc), "cmode", board->cmode,
                             &error_fatal);
    qdev_connect_clock_in(DEVICE(&board->soc), "osc", board->osc_clk);
    if (!sysbus_realize(SYS_BUS_DEVICE(&board->soc), &error_fatal)) {
        return;
    }
}

static void v832_board_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "NEC V832 development board";
    mc->init = v832_board_init;
    mc->default_cpu_type = "v832-v830-cpu";
    mc->default_ram_size = 16 * MiB;
    mc->default_ram_id = "v832-board.ram";
    object_class_property_add(oc, "cmode", "uint8",
                              v832_board_get_cmode,
                              v832_board_set_cmode, NULL, NULL);
    object_class_property_set_description(oc, "cmode",
                                          "V832 CMODE strap: 0 selects 6x, 1 selects 8x");
}

static const TypeInfo v832_board_types[] = {
{
    .name = TYPE_V832_BOARD,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(V832BoardState),
    .instance_init = v832_board_instance_init,
    .class_init = v832_board_class_init,
},
};

DEFINE_TYPES(v832_board_types)
