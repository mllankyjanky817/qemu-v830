#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/audio.h"
#include "qemu/timer.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "hw/v830/v832_soc.h"

#define V832_SOUND_RAM_MAX_SIZE (32 * MiB)
#define V832_SOUND_EXTERNAL_SIZE (128 * MiB)
#define V832_SOUND_ROM_BASE 0x07000000u
#define V832_SOUND_ROM_SIZE (16 * MiB)
#define V832_SOUND_DAC_BASE 0x30000000u
#define V832_SOUND_DAC_SIZE 0x4
#define V832_SOUND_DAC_SAMPLE_RATE 44.1E3
#define V832_SOUND_DMA_BLOCK_SAMPLES 2048
#define V832_SOUND_AUDIO_BATCH_SAMPLES 8 * KiB
#define V832_SOUND_AUDIO_BUFFER_SAMPLES \
    (V832_SOUND_AUDIO_BATCH_SAMPLES * 8)
#define V832_SOUND_EXTERNAL_BLOCK_SIZE (V832_SOUND_EXTERNAL_SIZE / 8)
#define V832_SOUND_IMAGE_COUNT \
    (UINT32_MAX / V832_SOUND_EXTERNAL_BLOCK_SIZE + 1)

typedef struct V832SoundBoardState {
    MachineState parent_obj;
    V832SoCState soc;
    Clock *osc_clk;
    MemoryRegion external;
    MemoryRegion external_alias[V832_SOUND_IMAGE_COUNT];
    MemoryRegion rom;
    MemoryRegion dac;
    AudioBackend *audio_be;
    SWVoiceOut *audio_voice;
    int16_t audio_fifo[V832_SOUND_AUDIO_BUFFER_SAMPLES];
    int16_t audio_batch[V832_SOUND_AUDIO_BATCH_SAMPLES];
    size_t audio_fifo_read;
    size_t audio_fifo_write;
    size_t audio_fifo_samples;
    QEMUTimer dac_timer;
    qemu_irq dma_request;
    uint16_t dac_sample;
    uint64_t dac_samples;
} V832SoundBoardState;

#define TYPE_V832_SOUND_BOARD MACHINE_TYPE_NAME("v832_sound_board")
OBJECT_DECLARE_SIMPLE_TYPE(V832SoundBoardState, V832_SOUND_BOARD)

static void v832_sound_audio_flush(V832SoundBoardState *board)
{
    size_t written;
    size_t first;
    int16_t *batch;

    while (board->audio_fifo_samples >= V832_SOUND_AUDIO_BATCH_SAMPLES) {
        first = MIN(V832_SOUND_AUDIO_BATCH_SAMPLES,
                    V832_SOUND_AUDIO_BUFFER_SAMPLES -
                    board->audio_fifo_read);
        if (first == V832_SOUND_AUDIO_BATCH_SAMPLES) {
            batch = &board->audio_fifo[board->audio_fifo_read];
        } else {
            memcpy(board->audio_batch,
                   &board->audio_fifo[board->audio_fifo_read],
                   first * sizeof(int16_t));
            memcpy(board->audio_batch + first, board->audio_fifo,
                   (V832_SOUND_AUDIO_BATCH_SAMPLES - first) *
                   sizeof(int16_t));
            batch = board->audio_batch;
        }
        written = audio_be_write(board->audio_be, board->audio_voice,
                                 batch,
                                 V832_SOUND_AUDIO_BATCH_SAMPLES *
                                 sizeof(int16_t));
        written /= sizeof(int16_t);
        if (!written) {
            break;
        }
        board->audio_fifo_read =
            (board->audio_fifo_read + written) %
            V832_SOUND_AUDIO_BUFFER_SAMPLES;
        board->audio_fifo_samples -= written;
    }
}

static void v832_sound_audio_callback(void *opaque, int avail)
{
    V832SoundBoardState *board = opaque;

    v832_sound_audio_flush(board);
}

static uint64_t v832_sound_dac_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    V832SoundBoardState *board = opaque;

    return size == 2 && offset == 0 ? board->dac_sample : 0;
}

static void v832_sound_dac_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    V832SoundBoardState *board = opaque;

    if (offset == 0 && size == 2) {
        int16_t sample = (int16_t)(value ^ 0x8000);

        board->dac_sample = value;
        board->dac_samples++;
        if (board->audio_fifo_samples < V832_SOUND_AUDIO_BUFFER_SAMPLES) {
            board->audio_fifo[board->audio_fifo_write] = sample;
            board->audio_fifo_write =
                (board->audio_fifo_write + 1) %
                V832_SOUND_AUDIO_BUFFER_SAMPLES;
            board->audio_fifo_samples++;
            if (board->audio_fifo_samples >=
                V832_SOUND_AUDIO_BATCH_SAMPLES) {
                v832_sound_audio_flush(board);
            }
        }
    }
}

static const MemoryRegionOps v832_sound_dac_ops = {
    .read = v832_sound_dac_read,
    .write = v832_sound_dac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 2,
};

static void v832_sound_dac_tick(void *opaque)
{
    V832SoundBoardState *board = opaque;
    uint64_t period = ((uint64_t)NANOSECONDS_PER_SECOND *
                       V832_SOUND_DMA_BLOCK_SAMPLES) /
                      V832_SOUND_DAC_SAMPLE_RATE;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);

    /* The firmware programs one 1024-sample block per DMA terminal count. */
    for (unsigned sample = 0;
         sample < V832_SOUND_DMA_BLOCK_SAMPLES; sample++) {
        qemu_set_irq(board->dma_request, 1);
        qemu_set_irq(board->dma_request, 0);
    }
    timer_mod(&board->dac_timer, now + period);
}

static void v832_sound_board_init(MachineState *machine)
{
    V832SoundBoardState *board = V832_SOUND_BOARD(machine);
    MemoryRegion *sysmem = get_system_memory();

    if (machine->ram_size == 0 || machine->ram_size >= V832_SOUND_RAM_MAX_SIZE) {
        error_report("V832 sound board RAM must be less than 32 MiB");
        exit(EXIT_FAILURE);
    }

    memory_region_init(&board->external, OBJECT(machine), "v832-sound-external",
                       V832_SOUND_EXTERNAL_SIZE);
    memory_region_add_subregion(&board->external, 0, machine->ram);
    memory_region_init_ram(&board->rom, NULL, "v832-sound-rom",
                           V832_SOUND_ROM_SIZE, &error_fatal);
    if (machine->firmware) {
        gchar *image_data = NULL;
        gsize image_size;
        GError *error = NULL;

        if (!g_file_get_contents(machine->firmware, &image_data, &image_size,
                                 &error) || image_size == 0 ||
            image_size > V832_SOUND_ROM_SIZE) {
            error_report("could not load V832 sound-board firmware '%s': %s",
                         machine->firmware,
                         error ? error->message : "invalid image size");
            g_clear_error(&error);
            g_free(image_data);
            exit(EXIT_FAILURE);
        }
        for (size_t offset = 0; offset < V832_SOUND_ROM_SIZE;
             offset += image_size) {
            size_t copy_size = MIN(image_size, V832_SOUND_ROM_SIZE - offset);

            memcpy(memory_region_get_ram_ptr(&board->rom) + offset,
                   image_data, copy_size);
        }
        g_free(image_data);
    }
    memory_region_set_readonly(&board->rom, true);
    memory_region_add_subregion(&board->external, V832_SOUND_ROM_BASE,
                                &board->rom);

    for (unsigned i = 8; i < V832_SOUND_IMAGE_COUNT; i++) {
        memory_region_init_alias(&board->external_alias[i], OBJECT(machine),
                                 "v832-sound-external-high-alias",
                                 &board->external,
                                 (i % 8) * V832_SOUND_EXTERNAL_BLOCK_SIZE,
                                 V832_SOUND_EXTERNAL_BLOCK_SIZE);
        memory_region_add_subregion(sysmem,
                                    (hwaddr)i * V832_SOUND_EXTERNAL_BLOCK_SIZE,
                                    &board->external_alias[i]);
    }

    board->osc_clk = clock_new(OBJECT(machine), "osc");
    clock_set_hz(board->osc_clk, 23800000);
    object_initialize_child(OBJECT(machine), "soc", &board->soc,
                            TYPE_V832_SOC);
    board->soc.external = &board->external;
    qdev_connect_clock_in(DEVICE(&board->soc), "osc", board->osc_clk);
    if (!sysbus_realize(SYS_BUS_DEVICE(&board->soc), &error_fatal)) {
        return;
    }

    if (machine->audiodev) {
        board->audio_be = audio_be_by_name(machine->audiodev, &error_fatal);
        if (!board->audio_be) {
            return;
        }
    } else if (!audio_be_check(&board->audio_be, &error_fatal)) {
        return;
    }
    {
        struct audsettings settings = {
            .freq = V832_SOUND_DAC_SAMPLE_RATE,
            .nchannels = 1,
            .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };

        board->audio_voice = audio_be_open_out(
            board->audio_be, NULL, "v832-sound-dac", board,
            v832_sound_audio_callback, &settings);
        if (!board->audio_voice) {
            error_setg(&error_fatal, "could not open V832 DAC audio voice");
            return;
        }
        audio_be_set_active_out(board->audio_be, board->audio_voice, true);
    }

    memory_region_init_io(&board->dac, OBJECT(machine), &v832_sound_dac_ops,
                          board, "v832-sound-dac", V832_SOUND_DAC_SIZE);
    memory_region_add_subregion_overlap(sysmem, V832_SOUND_DAC_BASE,
                                        &board->dac, 20);

    board->dma_request = qdev_get_gpio_in(DEVICE(&board->soc.dma), 0);
    timer_init_ns(&board->dac_timer, QEMU_CLOCK_VIRTUAL_RT,
                  v832_sound_dac_tick, board);
    timer_mod(&board->dac_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT) +
                  ((uint64_t)NANOSECONDS_PER_SECOND *
                   V832_SOUND_DMA_BLOCK_SAMPLES) /
                  V832_SOUND_DAC_SAMPLE_RATE);
}

static void v832_sound_board_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "NEC V832 sound board with 16-bit DMA-fed DAC";
    mc->init = v832_sound_board_init;
    mc->default_cpu_type = "v832-v830-cpu";
    mc->default_ram_size = 16 * MiB;
    mc->default_ram_id = "v832-sound-board.ram";
    machine_add_audiodev_property(mc);
}

static const TypeInfo v832_sound_board_type = {
    .name = TYPE_V832_SOUND_BOARD,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(V832SoundBoardState),
    .class_init = v832_sound_board_class_init,
};

static void v832_sound_board_register_types(void)
{
    type_register_static(&v832_sound_board_type);
}

type_init(v832_sound_board_register_types)