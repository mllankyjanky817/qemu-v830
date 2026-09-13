#ifndef HW_V830_V832_DMA_H
#define HW_V830_V832_DMA_H

#include "hw/core/sysbus.h"
#include "target/v830/cpu.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_V832_DMA "v832-dma"
OBJECT_DECLARE_SIMPLE_TYPE(V832DMAState, V832_DMA)

#define V832_DMA_CHANNELS 4
#define V832_DMA_MMIO_SIZE 0x40

enum V832DMARequest {
    V832_DMA_REQUEST_EXTERNAL = 0,
    V832_DMA_REQUEST_SOFTWARE = 1,
    V832_DMA_REQUEST_UART_TX = 4,
    V832_DMA_REQUEST_UART_RX = 5,
    V832_DMA_REQUEST_CSI = 6,
    V832_DMA_REQUEST_TIMER4 = 7,
};

typedef struct V832DMAChannel {
    uint32_t dsa;
    uint32_t dda;
    uint32_t dbc;
    uint16_t dchc;
    bool external_request;
    bool software_request;
    bool transfer_started;
} V832DMAChannel;

struct V832DMAState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    V830CPU *cpu;
    qemu_irq irq;

    V832DMAChannel channel[V832_DMA_CHANNELS];
    qemu_irq dmaak[V832_DMA_CHANNELS];
    qemu_irq tc_stopak;

    /* Pending UART/CSI/timer request, indexed by TTYP. */
    bool pending_internal[8];
    bool arbitrating;
    bool nmi_level;
    uint16_t dc;
    QEMUTimer request_timer;
};

void v832_dma_set_internal_request(V832DMAState *s, enum V832DMARequest request);
void v832_dma_nmi(V832DMAState *s);

#endif