#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/memory.h"
#include "hw/core/qdev-clock.h"
#include "target/v830/cpu.h"
#include "hw/v830/v832_peripherals.h"

#define V832_IO_BASE 0xc0000000u
#define V832_IO_SIZE 0x400

#define PORT  0x00
#define PM    0x02
#define PC    0x04
#define PORTA 0xf0
#define PAM   0xf2
#define PAC   0xf4
#define PORTB 0xf6
#define PBM   0xf8
#define PBC   0xfa

#define UART_ASIM00 0x90
#define UART_ASIM01 0x92
#define UART_ASIS0  0x94
#define UART_RXB0   0x98
#define UART_RXB0L  0x9a
#define UART_TXS0   0x9c
#define UART_TXS0L  0x9e
#define UART_BRG0   0xb0
#define UART_BPRM0  0xb2

#define CSI_CSIM0   0xa0
#define CSI_SIO0    0xa2

#define TOVS 0x70
#define TUM1 0x72
#define TMC1 0x74
#define TOC1 0x76
#define TM1  0x78
#define CC10 0x80
#define CC11 0x82
#define CC12 0x84
#define CC13 0x86
#define TMC4 0x88
#define TM4  0x8a
#define CM4  0x8c

#define IGP  0xc0
#define ICR  0xc2
#define IRR  0xc4
#define IMR  0xc6
#define IMOD 0xc8

#define ASIM00_RXE 0x40
#define ASIS0_SOT  0x80
#define ASIS0_PE   0x04
#define ASIS0_FE   0x02
#define ASIS0_OV   0x01
#define TMC1_CE    0x80
#define TMC4_CE    0x80
#define TMC1_OVIE  0x40
#define CSIM0_CTXE 0x80
#define CSIM0_CRXE 0x40
#define CSIM0_CSOT 0x20
#define CSIM0_MOD  0x10
#define CSIM0_CLS_MASK 0x07

#define V832_IRQ_UART_RX  11
#define V832_IRQ_UART_TX  10
#define V832_IRQ_UART_ERR 13
#define V832_IRQ_TIMER1   14
#define V832_IRQ_DMA      7
#define INTCM4_SOURCE     3
#define INTCSI_SOURCE     9

static const unsigned v832_intp_sources[8] = {
    0, 4, 8, 12, 6, 5, 2, 1,
};

static const int v832_portb_intp[8] = {
    -1, -1, 0, 2, 1, 5, 6, 3,
};

static unsigned v832_intp_mode(const V832PeripheralsState *s, unsigned n)
{
    return (s->imod >> (n * 2)) & 0x3;
}

static unsigned v832_interrupt_level(const V832PeripheralsState *s,
                                     unsigned source)
{
    unsigned group = source >> 2;
    unsigned group_priority = (s->igp >> (group * 2)) & 0x3;

    return (group_priority << 2) | (source & 0x3);
}

static bool v832_source_is_level(const V832PeripheralsState *s,
                                 unsigned source)
{
    unsigned pin;

    switch (source) {
    case 0: case 4: case 8: case 12:
        pin = source >> 2;
        return v832_intp_mode(s, pin) == 0;
    case 6: pin = 0; break;
    case 5: pin = 1; break;
    case 2: pin = 2; break;
    case 1: pin = 3; break;
    default:
        return false;
    }
    return ((s->imod >> (8 + pin * 2)) & 0x3) == 0;
}

static void v832_update_irq(V832PeripheralsState *s)
{
    uint16_t deliverable = s->irr & (uint16_t)~s->imr & 0x7fffu;
    unsigned level = 0;
    bool selected = false;

    if (deliverable != 0) {
        for (unsigned candidate = 0; candidate < 15; candidate++) {
            if (!(deliverable & BIT(candidate))) {
                continue;
            }
            if (!selected ||
                v832_interrupt_level(s, candidate) > level) {
                level = v832_interrupt_level(s, candidate);
                selected = true;
            }
        }


        if (s->cpu) {
            v830_cpu_set_interrupt_source(s->cpu, level);
            cpu_interrupt(CPU(s->cpu), CPU_INTERRUPT_HARD);
        }
        qemu_set_irq(s->irq, 1);
    } else {
        if (s->cpu) {
            cpu_reset_interrupt(CPU(s->cpu), CPU_INTERRUPT_HARD);
        }
        qemu_set_irq(s->irq, 0);
    }
}

static void v832_raise_irq(V832PeripheralsState *s, unsigned source)
{
    s->irr |= (uint16_t)(1u << source);
    v832_update_irq(s);
    qemu_log_mask(CPU_LOG_INT, "V832 IRQ source %u raised\n", source);
}

static void v832_peripherals_intp(void *opaque, int n, int level)
{
    V832PeripheralsState *s = opaque;
    unsigned source;
    unsigned mode;
    bool active;

    if (n < 0 || n >= ARRAY_SIZE(v832_intp_sources)) {
        return;
    }

    source = v832_intp_sources[n];
    mode = v832_intp_mode(s, n);
    active = mode == 3 ? level != s->intp_level[n]
                       : level && !s->intp_level[n];
    s->intp_level[n] = level;

    if (mode == 0) {
        if (level) {
            v832_raise_irq(s, source);
        } else {
            s->irr &= (uint16_t)~(1u << source);
            v832_update_irq(s);
        }
    } else if (active && (mode == 2 || mode == 3)) {
        v832_raise_irq(s, source);
    }
}

static void v832_peripherals_dma_irq(void *opaque, int n, int level)
{
    V832PeripheralsState *s = opaque;

    if (n == 0 && level) {
        v832_raise_irq(s, V832_IRQ_DMA);
    }
}

static void v832_update_port_outputs(V832PeripheralsState *s)
{
    for (unsigned bit = 0; bit < 5; bit++) {
        qemu_set_irq(s->port_out[bit],
                     !(s->pm & BIT(bit)) && !(s->pc & BIT(bit)) &&
                     !!(s->port & BIT(bit)));
    }
    for (unsigned bit = 0; bit < 8; bit++) {
        qemu_set_irq(s->porta_out[bit],
                     !(s->pam & BIT(bit)) &&
                     ((s->pac & BIT(bit)) ?
                      ((bit & 1) && !!(s->dmaak_level & BIT(bit / 2))) :
                      !!(s->porta & BIT(bit))));
        qemu_set_irq(s->portb_out[bit],
                     !(s->pbm & BIT(bit)) && !(s->pbc & BIT(bit)) &&
                     !!(s->portb & BIT(bit)));
    }
}

static void v832_port_in(void *opaque, int n, int level)
{
    V832PeripheralsState *s = opaque;

    if (n < 5) {
        if (level) {
            s->port_input |= BIT(n);
        } else {
            s->port_input &= ~BIT(n);
        }
    }
}

static void v832_porta_in(void *opaque, int n, int level)
{
    V832PeripheralsState *s = opaque;

    if (level) {
        s->porta_input |= BIT(n);
    } else {
        s->porta_input &= ~BIT(n);
    }
    if ((s->pac & BIT(n)) && !(n & 1)) {
        qemu_set_irq(s->dmarq_out[n / 2], level);
    }
}

static void v832_dmaak_in(void *opaque, int n, int level)
{
    V832PeripheralsState *s = opaque;

    if (n < 4) {
        if (level) {
            s->dmaak_level |= BIT(n);
        } else {
            s->dmaak_level &= ~BIT(n);
        }
        if (s->pac & BIT(2 * n + 1)) {
            qemu_set_irq(s->porta_out[2 * n + 1], level);
        }
    }
}

static void v832_portb_in(void *opaque, int n, int level)
{
    V832PeripheralsState *s = opaque;

    if (level) {
        s->portb_input |= BIT(n);
    } else {
        s->portb_input &= ~BIT(n);
    }
    if ((s->pbc & BIT(n)) && v832_portb_intp[n] >= 0) {
        v832_peripherals_intp(s, v832_portb_intp[n], level);
    }
}

static void v832_csi_raise_done(V832PeripheralsState *s)
{
    s->csim0 &= ~CSIM0_CSOT;
    v832_raise_irq(s, INTCSI_SOURCE);
    if (s->dma) {
        v832_dma_set_internal_request(s->dma, V832_DMA_REQUEST_CSI);
    }
}

static uint64_t v832_csi_half_period_ns(const V832PeripheralsState *s)
{
    unsigned cls = s->csim0 & CSIM0_CLS_MASK;
    unsigned divider;
    unsigned brg;

    if (cls == 0) {
        return 0;
    }
    if (cls == 1) {
        if (!(s->bprm0 & 0x80u)) {
            return 0;
        }
        brg = s->brg0 ? s->brg0 : 256;
        divider = 2u * brg;
        switch (s->bprm0 & 0x07u) {
        case 0: break;
        case 1: divider *= 2; break;
        case 2: divider *= 4; break;
        case 3: divider *= 8; break;
        default: divider *= 16; break;
        }
    } else {
        divider = 1u << (cls - 1);
        divider /= 2;
    }
    return clock_ticks_to_ns(s->clk, divider);
}

static void v832_csi_sample_rising(V832PeripheralsState *s)
{
    unsigned bit = s->csim0 & CSIM0_MOD ? s->csi_bit : 7 - s->csi_bit;

    if (s->csim0 & CSIM0_CRXE) {
        if (s->csi_si_level) {
            s->sio0 |= 1u << bit;
        } else {
            s->sio0 &= ~(1u << bit);
        }
    }
    s->csi_bit++;
    if (s->csi_bit == 8) {
        s->csi_active = false;
        timer_del(&s->csi_timer);
        v832_csi_raise_done(s);
    }
}

static void v832_csi_drive_falling(V832PeripheralsState *s)
{
    unsigned bit = s->csim0 & CSIM0_MOD ? s->csi_bit : 7 - s->csi_bit;

    qemu_set_irq(s->csi_so_out,
                 (s->csim0 & CSIM0_CTXE) ? !!(s->sio0 & (1u << bit)) : 0);
}

static void v832_csi_clock_edge(V832PeripheralsState *s, int level)
{
    if (!s->csi_active || (s->csim0 & CSIM0_CLS_MASK) != 0) {
        return;
    }
    if (level) {
        v832_csi_sample_rising(s);
    } else {
        v832_csi_drive_falling(s);
    }
}

static void v832_csi_sclk_in(void *opaque, int n, int level)
{
    V832PeripheralsState *s = opaque;

    if (n != 0 || level == s->csi_sclk_level) {
        return;
    }
    s->csi_sclk_level = level;
    v832_csi_clock_edge(s, level);
}

static void v832_csi_si_in(void *opaque, int n, int level)
{
    V832PeripheralsState *s = opaque;

    if (n == 0) {
        s->csi_si_level = level;
    }
}

static void v832_csi_tick(void *opaque)
{
    V832PeripheralsState *s = opaque;
    uint64_t half_period;
    bool external_clock = (s->csim0 & CSIM0_CLS_MASK) == 0;

    if (!s->csi_active || (s->csim0 & CSIM0_CLS_MASK) == 0) {
        return;
    }
    s->csi_sclk_level = !s->csi_sclk_level;
    qemu_set_irq(s->csi_sclk_out, s->csi_sclk_level);
    if (external_clock) {
        if (s->csi_sclk_level) {
            v832_csi_sample_rising(s);
        } else {
            v832_csi_drive_falling(s);
        }
    } else if (s->csi_sclk_level) {
        s->csi_bit++;
        if (s->csi_bit == 8) {
            s->csi_active = false;
            timer_del(&s->csi_timer);
            v832_csi_raise_done(s);
        }
    }
    if (!s->csi_active) {
        qemu_set_irq(s->csi_sclk_out, 0);
        return;
    }
    half_period = v832_csi_half_period_ns(s);
    timer_mod(&s->csi_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + half_period);
}

static void v832_csi_start(V832PeripheralsState *s)
{
    unsigned cls = s->csim0 & CSIM0_CLS_MASK;

    if (s->csi_active || (!(s->csim0 & CSIM0_CTXE) &&
                          !(s->csim0 & CSIM0_CRXE))) {
        return;
    }
    s->csi_active = true;
    s->csi_bit = 0;
    s->csim0 |= CSIM0_CSOT;
    s->csi_sclk_level = 0;
    if (cls != 0) {
        uint8_t tx = (s->csim0 & CSIM0_CTXE) ? s->sio0 : 0;
        uint8_t rx = ssi_transfer(s->csi_bus, tx);

        if (s->csim0 & CSIM0_CRXE) {
            s->sio0 = rx;
        }
    } else if (s->csim0 & CSIM0_CTXE) {
        v832_csi_drive_falling(s);
    }
    if (cls != 0) {
            uint64_t half_period = v832_csi_half_period_ns(s);

            if (!half_period) {
                s->csi_active = false;
                s->csim0 &= ~CSIM0_CSOT;
                return;
            }
            timer_mod(&s->csi_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + half_period);
    }
}

static void v832_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    V832PeripheralsState *s = opaque;

    if (!(s->asim00 & ASIM00_RXE) || size <= 0) {
        return;
    }
    if (s->rxb0 & 0xff) {
        s->asis0 |= ASIS0_OV;
    }
    s->rxb0 = buf[0];
    v832_raise_irq(s, V832_IRQ_UART_RX);
    if (s->dma) {
        v832_dma_set_internal_request(s->dma, V832_DMA_REQUEST_UART_RX);
    }
}

static int v832_uart_can_receive(void *opaque)
{
    V832PeripheralsState *s = opaque;

    return (s->asim00 & ASIM00_RXE) != 0;
}

static void v832_uart_event(void *opaque, QEMUChrEvent event)
{
    V832PeripheralsState *s = opaque;

    if (event == CHR_EVENT_BREAK) {
        s->asis0 |= ASIS0_FE;
        v832_raise_irq(s, V832_IRQ_UART_ERR);
    }
}

static uint64_t v832_timer1_tick_ns(const V832PeripheralsState *s)
{
    unsigned hz = clock_get_hz(s->clk);
    uint32_t divider;

    if (!hz) {
        return NANOSECONDS_PER_SECOND / 1000;
    }

    /*
     * Timer 1 uses the bus clock f and selects the count clock as:
     *   fm = f / 2      when PRM11 = 0
     *   fm = f / 4      when PRM11 = 1
     *   final count = fm, fm/4, fm/16 depending PRS11/PRS10.
     *
     * The V832 manual defines the valid selections as:
     *   00 -> fm
     *   01 -> fm/4
     *   11 -> fm/16
     */
    if (s->tmc1 & 0x02u) {
        divider = 4u; /* fm = f/4 */
    } else {
        divider = 2u; /* fm = f/2 */
    }

    switch ((s->tmc1 >> 2) & 0x03u) {
    case 0x00u:
        break;
    case 0x01u:
        divider *= 4u;
        break;
    case 0x03u:
        divider *= 16u;
        break;
    default:
        divider *= 16u;
        break;
    }

    return clock_ticks_to_ns(s->clk, divider);
}

static uint64_t v832_timer4_tick_ns(const V832PeripheralsState *s)
{
    unsigned hz = clock_get_hz(s->clk);
    uint32_t divider;

    if (!hz) {
        return NANOSECONDS_PER_SECOND / 1000;
    }

    /* TM4 uses fm = f/2 or f/8, then divides by 16 or 32. */
    if ((s->tmc4 & 0x03u) == 0x02u) {
        divider = 8u;
    } else {
        divider = 2u;
    }

    if (s->tmc4 & 0x04u) {
        divider *= 32u;
    } else {
        divider *= 16u;
    }

    return clock_ticks_to_ns(s->clk, divider);
}

static void v832_timer4_schedule(V832PeripheralsState *s);

static uint64_t v832_elapsed_ticks(const V832PeripheralsState *s,
                                   uint64_t elapsed_ns, uint32_t divider)
{
    unsigned hz = clock_get_hz(s->clk);

    if (!hz) {
        return 0;
    }

    return muldiv64(elapsed_ns, hz,
                    (uint64_t)divider * NANOSECONDS_PER_SECOND);
}

static void v832_timer1_sync(V832PeripheralsState *s, uint64_t now_ns)
{
    uint32_t divider = (s->tmc1 & 0x02u) ? 4u : 2u;
    uint64_t elapsed_ns;
    uint64_t ticks;

    if (!(s->tmc1 & TMC1_CE) || now_ns <= s->timer1_last_ns) {
        return;
    }

    elapsed_ns = now_ns - s->timer1_last_ns;
    ticks = v832_elapsed_ticks(s, elapsed_ns, divider);
    if (!ticks) {
        return;
    }

    while (ticks--) {
        unsigned index;

        s->tm1++;
        if (s->tm1 == 0) {
            s->tovs |= 1u << 1;
            if (s->tmc1 & TMC1_OVIE) {
                v832_raise_irq(s, V832_IRQ_TIMER1);
            }
        }
        for (index = 0; index < 4; index++) {
            if ((s->tum1 & (1u << (4 + index))) &&
                !(s->tum1 & (1u << index)) &&
                s->tm1 == s->cc[index]) {
                v832_raise_irq(s, v832_intp_sources[4 + index]);
            }
        }
    }
    s->timer1_last_ns = now_ns;
}

static void v832_timer4_sync(V832PeripheralsState *s, uint64_t now_ns)
{
    uint32_t divider = ((s->tmc4 & 0x03u) == 0x02u) ? 8u : 2u;
    uint32_t tick_divider = divider * ((s->tmc4 & 0x04u) ? 32u : 16u);
    uint64_t elapsed_ns;
    uint64_t ticks;

    if (!(s->tmc4 & TMC4_CE) || now_ns <= s->timer4_last_ns) {
        return;
    }

    elapsed_ns = now_ns - s->timer4_last_ns;
    ticks = v832_elapsed_ticks(s, elapsed_ns, tick_divider);
    if (!ticks) {
        return;
    }

    s->timer4_last_ns += clock_ticks_to_ns(s->clk, ticks * tick_divider);

    if (s->timer4_clear_pending) {
        ticks--;
        s->tm4 = 0;
        s->timer4_clear_pending = false;
        if (ticks == 0) {
            v832_timer4_schedule(s);
            return;
        }
    }

    if (s->cm4 == 0) {
        uint64_t total = (uint64_t)s->tm4 + ticks;

        if (total >> 16) {
            s->tovs |= 1u << 4;
        }
        s->tm4 = total;
        return;
    }

    while (ticks != 0) {
        uint64_t to_compare = s->cm4 - s->tm4;

        if (ticks < to_compare) {
            s->tm4 += ticks;
            break;
        }

        s->tm4 = s->cm4;
        ticks -= to_compare;
        v832_raise_irq(s, INTCM4_SOURCE);
        if (s->dma) {
            v832_dma_set_internal_request(s->dma, V832_DMA_REQUEST_TIMER4);
        }
        if (ticks == 0) {
            s->timer4_clear_pending = true;
            if (s->dma) {
                v832_dma_set_internal_request(s->dma,
                                              V832_DMA_REQUEST_TIMER4);
            }
            break;
        }

        ticks--;
        s->tm4 = 0;
        s->timer4_clear_pending = false;
    }
}

static void v832_timer1_schedule(V832PeripheralsState *s)
{
    uint64_t ticks = UINT16_MAX - s->tm1 + 1;
    unsigned index;

    for (index = 0; index < 4; index++) {
        uint64_t compare_ticks;

        if (!(s->tum1 & (1u << (4 + index))) ||
            (s->tum1 & (1u << index))) {
            continue;
        }
        compare_ticks = (uint16_t)(s->cc[index] - s->tm1);
        if (compare_ticks == 0) {
            compare_ticks = UINT16_MAX + 1ULL;
        }
        ticks = MIN(ticks, compare_ticks);
    }

    timer_mod(&s->timer1, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ticks * v832_timer1_tick_ns(s));
}

static void v832_timer4_schedule(V832PeripheralsState *s)
{
    uint64_t ticks;

    if (s->timer4_clear_pending) {
        ticks = 1;
    } else if (s->cm4 != 0) {
        ticks = s->cm4 > s->tm4 ? s->cm4 - s->tm4
                                : UINT16_MAX - s->tm4 + s->cm4 + 1;
    } else {
        ticks = UINT16_MAX - s->tm4 + 1;
    }

    timer_mod(&s->timer4, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ticks * v832_timer4_tick_ns(s));
}

static void v832_timer1_tick(void *opaque)
{
    V832PeripheralsState *s = opaque;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!(s->tmc1 & TMC1_CE)) {
        return;
    }
    v832_timer1_sync(s, now_ns);
    v832_timer1_schedule(s);
}

static void v832_timer4_tick(void *opaque)
{
    V832PeripheralsState *s = opaque;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!(s->tmc4 & TMC4_CE)) {
        return;
    }

    qemu_log_mask(CPU_LOG_INT,
                  "V832 TIMER4: virtual=%" PRId64
                  " tm4=%04x cm4=%04x clear_pending=%d\n",
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->tm4, s->cm4,
                  s->timer4_clear_pending);

    if (s->timer4_clear_pending) {
        s->tm4 = 0;
        s->timer4_clear_pending = false;
        s->timer4_last_ns = now_ns;
    } else {
        if (s->cm4 != 0) {
            s->tm4 = s->cm4;
            s->timer4_clear_pending = true;
            v832_raise_irq(s, INTCM4_SOURCE);
        } else {
            s->tm4 = 0;
            s->tovs |= 1u << 4;
            s->timer4_last_ns = now_ns;
        }
    }
    v832_timer4_schedule(s);
}

static uint64_t v832_peripherals_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    V832PeripheralsState *s = opaque;

    switch (offset) {
    case PORT:  return (s->port & ~s->pm) | (s->port_input & s->pm);
    case PM:    return s->pm | 0xe0;
    case PC:    return s->pc;
    case UART_ASIM00: return s->asim00;
    case UART_ASIM01: return s->asim01;
    case UART_ASIS0: return s->asis0;
    case UART_RXB0: return s->rxb0;
    case UART_RXB0L: return s->rxb0 & 0xff;
    case UART_BRG0: return s->brg0;
    case UART_BPRM0: return s->bprm0;
    case CSI_CSIM0: return s->csim0;
    case CSI_SIO0:
        if ((s->csim0 & CSIM0_CRXE) &&
            !(s->csim0 & CSIM0_CTXE)) {
            v832_csi_start(s);
        }
        return s->sio0;
    case TOVS: return s->tovs;
    case TUM1: return s->tum1;
    case TMC1: return s->tmc1;
    case TOC1: return s->toc1;
    case TM1:
        v832_timer1_sync(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        return s->tm1;
    case CC10: case CC11: case CC12: case CC13:
        return s->cc[(offset - CC10) / 2];
    case TMC4: return s->tmc4;
    case TM4:
        v832_timer4_sync(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        return s->tm4;
    case CM4: return s->cm4;
    case PORTA: return (s->porta & ~s->pam) | (s->porta_input & s->pam);
    case PAM:   return s->pam;
    case PAC:   return s->pac;
    case PORTB: return (s->portb & ~s->pbm) | (s->portb_input & s->pbm);
    case PBM:   return s->pbm;
    case PBC:   return s->pbc;
    case IGP: return s->igp;
    case IRR: return s->irr;
    case IMR: return s->imr;
    case IMOD: return s->imod;
    default:
        return 0;
    }
}

static void v832_peripherals_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    V832PeripheralsState *s = opaque;
    uint8_t byte = value;

    switch (offset) {
    case PORT:
        s->port = byte & 0x1f;
        v832_update_port_outputs(s);
        return;
    case PM:
        s->pm = byte & 0x1f;
        v832_update_port_outputs(s);
        return;
    case PC:
        s->pc = byte & 0x1f;
        v832_update_port_outputs(s);
        return;
    case UART_ASIM00: s->asim00 = byte & 0x7f; return;
    case UART_ASIM01: s->asim01 = byte & 1; return;
    case UART_ASIS0:
        s->asis0 &= ~(byte & (ASIS0_PE | ASIS0_FE | ASIS0_OV));
        return;
    case UART_TXS0: case UART_TXS0L:
        s->txs0 = value & 0x1ff;
        s->tx_busy = true;
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            uint8_t ch = s->txs0;
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        s->tx_busy = false;
        v832_raise_irq(s, V832_IRQ_UART_TX);
        if (s->dma) {
            v832_dma_set_internal_request(s->dma, V832_DMA_REQUEST_UART_TX);
        }
        return;
    case UART_BRG0: s->brg0 = byte; return;
    case UART_BPRM0: s->bprm0 = byte & 0x87; return;
    case CSI_CSIM0:
        if (s->csi_active && !(byte & CSIM0_CTXE) &&
            !(byte & CSIM0_CRXE)) {
            s->csi_active = false;
            timer_del(&s->csi_timer);
        }
        s->csim0 = byte & (CSIM0_CTXE | CSIM0_CRXE | CSIM0_MOD |
                           CSIM0_CLS_MASK);
        return;
    case CSI_SIO0:
        s->sio0 = byte;
        v832_csi_start(s);
        return;
    case TOVS:
        s->tovs &= byte;
        return;
    case IGP:
        s->igp = value;
        v832_update_irq(s);
        return;
    case ICR:
        for (unsigned source = 0; source < 15; source++) {
            if ((value & BIT(source)) &&
                !v832_source_is_level(s, source)) {
                s->irr &= (uint16_t)~BIT(source);
            }
        }
        v832_update_irq(s);
        return;
    case IMR:
        if (size == 1) {
            s->imr = (s->imr & 0xff00u) | byte;
        } else {
            s->imr = value & 0x7fffu;
        }
        v832_update_irq(s);
        return;
    case IMOD:
        s->imod = value;
        return;
    case TUM1: s->tum1 = value; return;
    case TMC1:
        s->tmc1 = value;
        if (!(s->tmc1 & TMC1_CE)) {
            s->tm1 = 0;
            timer_del(&s->timer1);
        } else if (!timer_pending(&s->timer1)) {
            s->timer1_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            v832_timer1_schedule(s);
        }
        return;
    case TOC1: s->toc1 = byte; return;
    case TM1: return;
    case CC10: case CC11: case CC12: case CC13:
        s->cc[(offset - CC10) / 2] = value;
        return;
    case TMC4:
        s->tmc4 = value & 0x87u;
        if (!(s->tmc4 & TMC4_CE)) {
            s->tm4 = 0;
            s->timer4_clear_pending = false;
            timer_del(&s->timer4);
        } else if (!timer_pending(&s->timer4)) {
            s->timer4_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            v832_timer4_schedule(s);
        }
        return;
    case TM4: return;
    case CM4:
        v832_timer4_sync(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        s->cm4 = value;
        if (s->tmc4 & TMC4_CE) {
            v832_timer4_schedule(s);
        }
        return;
    case PORTA:
        s->porta = byte;
        v832_update_port_outputs(s);
        return;
    case PAM:
        s->pam = byte;
        v832_update_port_outputs(s);
        return;
    case PAC:
        s->pac = byte;
        v832_update_port_outputs(s);
        return;
    case PORTB:
        s->portb = byte;
        v832_update_port_outputs(s);
        return;
    case PBM:
        s->pbm = byte;
        v832_update_port_outputs(s);
        return;
    case PBC:
        s->pbc = byte;
        v832_update_port_outputs(s);
        return;
    default: return;
    }
}

static const MemoryRegionOps v832_peripherals_ops = {
    .read = v832_peripherals_read,
    .write = v832_peripherals_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static void v832_peripherals_reset(DeviceState *dev)
{
    V832PeripheralsState *s = V832_PERIPHERALS(dev);

    s->asim00 = 0x80;
    s->asim01 = 0;
    s->asis0 = 0;
    s->rxb0 = 0;
    s->txs0 = 0;
    s->brg0 = 0;
    s->bprm0 = 0;
    s->csim0 = 0;
    s->sio0 = 0;
    s->tm1 = 0;
    s->tm4 = 0;
    s->cm4 = 0xffff;
    s->igp = 0x00e4;
    s->irr = 0;
    s->imr = 0xffff;
    s->imod = 0xaaaa;
    memset(s->intp_level, 0, sizeof(s->intp_level));
    s->tmc1 = 0;
    s->tmc4 = 0;
    s->tovs = 0;
    s->port = 0;
    s->pm = 0xff;
    s->pc = 0;
    s->porta = 0;
    s->pam = 0xff;
    s->pac = 0;
    s->portb = 0;
    s->pbm = 0xff;
    s->pbc = 0;
    s->port_input = 0;
    s->porta_input = 0;
    s->portb_input = 0;
    s->dmaak_level = 0;
    s->timer4_clear_pending = false;
    s->timer1_last_ns = 0;
    s->timer4_last_ns = 0;
    s->csi_bit = 0;
    s->csi_sclk_level = 0;
    s->csi_active = false;
    s->csi_si_level = false;
}

static void v832_peripherals_init(Object *obj)
{
    V832PeripheralsState *s = V832_PERIPHERALS(obj);

    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);
    s->csi_bus = ssi_create_bus(DEVICE(obj), "csi");
}

static void v832_peripherals_realize(DeviceState *dev, Error **errp)
{
    V832PeripheralsState *s = V832_PERIPHERALS(dev);

    if (!clock_has_source(s->clk)) {
        error_setg(errp, "clk clock must be wired up by the board code");
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &v832_peripherals_ops, s,
                          "v832-internal-io", V832_IO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_in_named(dev, v832_peripherals_dma_irq, "dma-irq", 1);
    qdev_init_gpio_in_named(dev, v832_peripherals_intp, "intp", 8);
    qdev_init_gpio_in_named(dev, v832_port_in, "port-in", 5);
    qdev_init_gpio_in_named(dev, v832_porta_in, "porta-in", 8);
    qdev_init_gpio_in_named(dev, v832_portb_in, "portb-in", 8);
    qdev_init_gpio_in_named(dev, v832_dmaak_in, "dmaak-in", 4);
    qdev_init_gpio_out_named(dev, s->port_out, "port-out", 5);
    qdev_init_gpio_out_named(dev, s->porta_out, "porta-out", 8);
    qdev_init_gpio_out_named(dev, s->portb_out, "portb-out", 8);
    qdev_init_gpio_out_named(dev, s->dmarq_out, "dmarq-out", 4);
    qdev_init_gpio_in_named(dev, v832_csi_sclk_in, "sclk-in", 1);
    qdev_init_gpio_in_named(dev, v832_csi_si_in, "si", 1);
    qdev_init_gpio_out_named(dev, &s->csi_sclk_out, "sclk-out", 1);
    qdev_init_gpio_out_named(dev, &s->csi_so_out, "so", 1);
    qemu_chr_fe_set_handlers(&s->chr, v832_uart_can_receive,
                             v832_uart_receive, v832_uart_event, NULL, s,
                             NULL, true);
    timer_init_ns(&s->timer1, QEMU_CLOCK_VIRTUAL, v832_timer1_tick, s);
    timer_init_ns(&s->timer4, QEMU_CLOCK_VIRTUAL, v832_timer4_tick, s);
    timer_init_ns(&s->csi_timer, QEMU_CLOCK_VIRTUAL, v832_csi_tick, s);
}

static const Property v832_peripherals_properties[] = {
    DEFINE_PROP_CHR("chardev", V832PeripheralsState, chr),
};

static void v832_peripherals_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, v832_peripherals_properties);
    dc->realize = v832_peripherals_realize;
    device_class_set_legacy_reset(dc, v832_peripherals_reset);
}

static const TypeInfo v832_peripherals_types[] = {
{
        .name = TYPE_V832_PERIPHERALS,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(V832PeripheralsState),
        .instance_init = v832_peripherals_init,
        .class_init = v832_peripherals_class_init,
},
};

DEFINE_TYPES(v832_peripherals_types)
