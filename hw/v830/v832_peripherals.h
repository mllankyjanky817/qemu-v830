#ifndef HW_V830_V832_PERIPHERALS_H
#define HW_V830_V832_PERIPHERALS_H

#define TYPE_V832_PERIPHERALS "v832-peripherals"

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "chardev/char-fe.h"
#include "qemu/timer.h"
#include "hw/ssi/ssi.h"
#include "hw/v830/v832_dma.h"

typedef struct V832PeripheralsState {
	SysBusDevice parent_obj;
	MemoryRegion iomem;
	CharFrontend chr;
	qemu_irq irq;
	qemu_irq csi_sclk_out;
	qemu_irq csi_so_out;
	qemu_irq port_out[5];
	qemu_irq porta_out[8];
	qemu_irq portb_out[8];
	qemu_irq dmarq_out[4];
	SSIBus *csi_bus;
	V830CPU *cpu;
	V832DMAState *dma;
	Clock *clk;
	uint8_t asim00, asim01, asis0, csim0, sio0;
	uint16_t rxb0, txs0;
	uint8_t brg0, bprm0;
	bool tx_busy;
	uint16_t tm1, tm4, cc[4], cm4, tum1;
	uint16_t igp, irr, imr, imod;
	uint8_t intp_level[8];
	uint8_t csi_bit;
	int csi_sclk_level;
	bool csi_active;
	bool csi_si_level;
	uint8_t tmc1, toc1, tmc4, tovs;
	uint8_t port, pm, pc;
	uint8_t porta, pam, pac;
	uint8_t portb, pbm, pbc;
	uint8_t port_input, porta_input, portb_input;
	uint8_t dmaak_level;
	bool timer4_clear_pending;
	uint64_t timer1_last_ns, timer4_last_ns;
	QEMUTimer timer1, timer4, csi_timer;
} V832PeripheralsState;

OBJECT_DECLARE_SIMPLE_TYPE(V832PeripheralsState, V832_PERIPHERALS)

#endif
