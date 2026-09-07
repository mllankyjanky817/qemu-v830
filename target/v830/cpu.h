#ifndef V830_CPU_H
#define V830_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"

#define CPU_RESOLVING_TYPE TYPE_V830_CPU
#define V830_NUM_GPRS 32
#define V830_CPU_IRQ_LINES 32
#define CPU_INTERRUPT_NMI CPU_INTERRUPT_TGT_EXT_3

#define V830_PSW_Z         (1u << 0)
#define V830_PSW_S         (1u << 1)
#define V830_PSW_OV        (1u << 2)
#define V830_PSW_CY        (1u << 3)
#define V830_PSW_FLAG_MASK (V830_PSW_Z | V830_PSW_S | V830_PSW_OV | \
                            V830_PSW_CY)
#define V830_PSW_SAT       (1u << 10)
#define V830_PSW_ID        (1u << 12)
#define V830_PSW_EP        (1u << 14)
#define V830_PSW_NP        (1u << 15)
#define V830_PSW_I_SHIFT   16
#define V830_PSW_I_MASK    (0xfu << V830_PSW_I_SHIFT)

enum {
    V830_EXCP_ILLEGAL = 0,
    V830_EXCP_DIV0,
    V830_EXCP_INTERRUPT,
    V830_EXCP_NMI,
};

enum V830MMUIndex {
    V830_MMU_DATA,
    V830_MMU_IO,
    V830_MMU_INTERNAL,
};

/* The V830 family is a 32-bit little-endian architecture. */
typedef struct CPUArchState {
    uint32_t regs[V830_NUM_GPRS];
    uint32_t pc;
    uint32_t zf;
    uint32_t sf;
    uint32_t ovf;
    uint32_t cyf;
    uint32_t psw;
    uint32_t eipc;
    uint8_t interrupt_source;
    uint32_t eipsw;
    uint32_t fepc;
    uint32_t fepsw;
    uint32_t ecr;
    uint32_t pir;
    uint32_t tkcw;
    uint32_t dpc;
    uint32_t dpsw;
    uint32_t hccw;
    uint32_t exception_index;
    bool nmi_level;
} V830CPUState;

static inline uint32_t v830_psw_read(const V830CPUState *env)
{
    return (env->psw & ~V830_PSW_FLAG_MASK) |
            (env->zf == 0 ? V830_PSW_Z : 0) |
            ((env->sf >> 31) ? V830_PSW_S : 0) |
            ((env->ovf >> 31) ? V830_PSW_OV : 0) |
            (env->cyf ? V830_PSW_CY : 0);
}

static inline void v830_psw_write(V830CPUState *env, uint32_t value)
{
    env->zf = (value & V830_PSW_Z) ? 0 : 1;
    env->sf = (value & V830_PSW_S) ? (1u << 31) : 0;
    env->ovf = (value & V830_PSW_OV) ? (1u << 31) : 0;
    env->cyf = !!(value & V830_PSW_CY);
    env->psw = value & ~V830_PSW_FLAG_MASK;
}

struct ArchCPU {
    CPUState parent_obj;
    V830CPUState env;
};

struct V830CPUClass {
    CPUClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

void v830_cpu_do_interrupt(CPUState *cs);
bool v830_cpu_exec_interrupt(CPUState *cs, int interrupt_request);
void v830_cpu_set_interrupt_source(V830CPU *cpu, unsigned source);
void v830_cpu_dump_state(CPUState *cs, FILE *f, int flags);
int v830_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int reg);
int v830_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int reg);
void v830_translate_init(void);
void v830_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc);

#endif
