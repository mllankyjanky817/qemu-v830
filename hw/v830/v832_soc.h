#ifndef HW_V830_V832_SOC_H
#define HW_V830_V832_SOC_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "system/memory.h"
#include "target/v830/cpu.h"
#include "hw/v830/v832_dma.h"
#include "hw/v830/v832_bcu.h"
#include "hw/v830/v832_peripherals.h"

#define TYPE_V832_SOC "v832-soc"

#define V832_SOC_IO_BASE V830_IO_VIRT_BASE
#define V832_SOC_INTERNAL_RAM_SIZE 0x1000
#define V832_SOC_INTERNAL_DATA_RAM_BASE 0x00000000
#define V832_SOC_INTERNAL_INSN_RAM_BASE 0xfe000000

typedef struct V832SoCState {
    SysBusDevice parent_obj;
    V830CPU cpu;
    V832BCUState bcu;
    V832PeripheralsState peripherals;
    V832DMAState dma;
    Clock *osc_clk;
    Clock *cpu_clk;
    Clock *bus_clk;
    uint8_t cmode;
    MemoryRegion *external;
    MemoryRegion internal_data_ram;
    MemoryRegion internal_insn_ram;
} V832SoCState;

OBJECT_DECLARE_SIMPLE_TYPE(V832SoCState, V832_SOC)

#endif
