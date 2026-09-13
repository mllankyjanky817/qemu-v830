#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "cpu.h"
#include "exec/cputlb.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "accel/tcg/cpu-ops.h"

static void v830_cpu_set_pc(CPUState *cs, vaddr value)
{
    cpu_env(cs)->pc = value;
}

static vaddr v830_cpu_get_pc(CPUState *cs)
{
    return cpu_env(cs)->pc;
}

static const char *v830_gdb_arch_name(CPUState *cs)
{
    return "v830";
}

static TCGTBCPUState v830_get_tb_cpu_state(CPUState *cs)
{
    V830CPUState *env = cpu_env(cs);
    return (TCGTBCPUState){ .pc = env->pc, .flags = v830_psw_read(env) };
}

static void v830_cpu_synchronize_from_tb(CPUState *cs,
                                         const TranslationBlock *tb)
{
    cpu_env(cs)->pc = tb->pc;
}

static void v830_restore_state_to_opc(CPUState *cs,
                                      const TranslationBlock *tb,
                                      const uint64_t *data)
{
    cpu_env(cs)->pc = data[0];
}

static bool v830_cpu_has_work(CPUState *cs)
{
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD | CPU_INTERRUPT_NMI);
}

static void v830_cpu_set_irq(void *opaque, int irq, int level)
{
    CPUState *cs = opaque;

    if (level) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static void v830_cpu_set_nmi(void *opaque, int irq, int level)
{
    V830CPUState *env = cpu_env(CPU(opaque));

    if (env->nmi_level && !level) {
        cpu_interrupt(CPU(opaque), CPU_INTERRUPT_NMI);
    }
    env->nmi_level = level;
}

static uint32_t v830_interrupt_vector_base(const V830CPUState *env)
{
    return (env->hccw & 1) ? 0xfe000000u : 0xfffffe00u;
}

void v830_cpu_set_interrupt_source(V830CPU *cpu, unsigned source)
{
    cpu->env.interrupt_source = source;
}

static int v830_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return ifetch ? V830_MMU_INTERNAL : V830_MMU_DATA;
}

static hwaddr v830_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    return addr;
}

static bool v830_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                              MMUAccessType access_type, int mmu_idx,
                              bool probe, uintptr_t retaddr)
{
    bool data_ram = (address & TARGET_PAGE_MASK) == 0;
    bool instruction_ram = (address & TARGET_PAGE_MASK) == 0xfe000000u;
    hwaddr physical_address = address;

    /* Internal RAM has separate instruction and data access paths. */
    if ((data_ram && access_type == MMU_INST_FETCH) ||
        (instruction_ram && mmu_idx == V830_MMU_DATA)) {
        if (probe) {
            return false;
        }
        cs->exception_index = V830_EXCP_ILLEGAL;
        cpu_loop_exit_restore(cs, retaddr);
    }

    if (mmu_idx == V830_MMU_IO &&
        address >= V830_IO_VIRT_BASE &&
        address < V830_IO_VIRT_BASE + V830_IO_MAP_SIZE) {
        physical_address = V830_IO_PHYS_BASE +
                           (address - V830_IO_VIRT_BASE);
    }

    tlb_set_page(cs, address & TARGET_PAGE_MASK,
                 physical_address & TARGET_PAGE_MASK,
                 PAGE_READ | PAGE_WRITE | PAGE_EXEC, mmu_idx,
                 TARGET_PAGE_SIZE);
    return true;
}

static void v830_cpu_realize(DeviceState *dev, Error **errp)
{
    V830CPUClass *vcc = V830_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_common_realize(CPU(dev), &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    qdev_init_gpio_out_named(dev, &V830_CPU(dev)->stopak, "stopak", 1);
    qemu_init_vcpu(CPU(dev));
    cpu_reset(CPU(dev));
    vcc->parent_realize(dev, errp);
}

static void v830_cpu_reset_hold(Object *obj, ResetType type)
{
    V830CPUClass *vcc = V830_CPU_GET_CLASS(obj);
    V830CPUState *env = cpu_env(CPU(obj));

    if (vcc->parent_phases.hold) {
        vcc->parent_phases.hold(obj, type);
    }
    memset(env, 0, offsetof(V830CPUState, exception_index));
    v830_psw_write(env, V830_PSW_NP);
    env->ecr = 0x0000fff0;
    env->pir = 0x00008302;
    env->tkcw = 0x000000e0;
    env->pc = 0xfffffff0u;
    env->interrupt_source = 0xff;
    env->nmi_level = false;
    qemu_set_irq(V830_CPU(obj)->stopak, 0);
}

void v830_cpu_do_interrupt(CPUState *cs)
{
    V830CPUState *env = cpu_env(cs);
    uint32_t cause;
    uint32_t handler;
    uint32_t exception_return_pc = env->pc + 2;

    if (cs->exception_index == V830_EXCP_NMI) {
        if (env->psw & V830_PSW_NP) {
            return;
        }
        cpu_reset_interrupt(cs, CPU_INTERRUPT_NMI);
        env->fepc = env->pc;
        env->fepsw = v830_psw_read(env);
        env->ecr = (env->ecr & 0xffffu) | (0xffd0u << 16);
        env->psw |= V830_PSW_NP | V830_PSW_ID;
        env->pc = 0xffffffd0u;
        cs->exception_index = -1;
        return;
    }

    if (cs->exception_index == V830_EXCP_INTERRUPT) {
        unsigned source = env->interrupt_source;

        if (source >= 16) {
            cs->exception_index = -1;
            return;
        }
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        cause = 0xfe00u + source * 0x10u;
        handler = v830_interrupt_vector_base(env) + source * 0x10u;
        env->eipsw = v830_psw_read(env);
        env->ecr = (env->ecr & 0xffff0000u) | cause;
        env->psw |= V830_PSW_EP | V830_PSW_ID;
        env->psw = (env->psw & ~V830_PSW_I_MASK) |
               (MIN(source + 1, 15u) << V830_PSW_I_SHIFT);
        env->eipc = env->pc;
        env->pc = handler;
        cs->exception_index = -1;
        return;
    }

    switch (cs->exception_index) {
    case V830_EXCP_ILLEGAL:
        cause = 0xff90;
        handler = 0xffffff90u;
        break;
    case V830_EXCP_DIV0:
        cause = 0xff80;
        handler = 0xffffff80u;
        break;
    default:
        return;
    }

    if (env->psw & V830_PSW_NP) {
        env->dpc = exception_return_pc;
        env->dpsw = v830_psw_read(env);
        env->psw |= V830_PSW_DP | V830_PSW_NP | V830_PSW_EP |
                    V830_PSW_ID;
        handler = 0xffffffe0u;
    } else if (env->psw & V830_PSW_EP) {
        env->fepc = exception_return_pc;
        env->fepsw = v830_psw_read(env);
        env->ecr = (env->ecr & 0xffff) | (cause << 16);
        env->psw |= V830_PSW_NP | V830_PSW_ID;
        handler = 0xffffffd0u;
    } else {
        env->eipc = exception_return_pc;
        env->eipsw = v830_psw_read(env);
        env->ecr = (env->ecr & 0xffff0000) | cause;
        env->psw |= V830_PSW_EP | V830_PSW_ID;
    }
    env->pc = handler;
    cs->exception_index = -1;
}

bool v830_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    V830CPUState *env = cpu_env(cs);
    unsigned source = env->interrupt_source;
    unsigned interrupt_level =
        (env->psw & V830_PSW_I_MASK) >> V830_PSW_I_SHIFT;

    if (interrupt_request) {
        qemu_set_irq(V830_CPU(cs)->stopak, 0);
    }

    if ((interrupt_request & CPU_INTERRUPT_NMI) &&
        !(env->psw & V830_PSW_NP)) {
        cs->exception_index = V830_EXCP_NMI;
        v830_cpu_do_interrupt(cs);
        return true;
    }

    if ((interrupt_request & CPU_INTERRUPT_HARD) &&
        !(env->psw & (V830_PSW_NP | V830_PSW_EP | V830_PSW_ID)) &&
        source < 16 && source >= interrupt_level) {
        cs->exception_index = V830_EXCP_INTERRUPT;
        v830_cpu_do_interrupt(cs);
        return true;
    }
    return false;
}

void v830_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    V830CPUState *env = cpu_env(cs);
    int i;

    qemu_fprintf(f, "PC: %08x PSW: %08x\n", env->pc, v830_psw_read(env));
    for (i = 0; i < V830_NUM_GPRS; i++) {
        qemu_fprintf(f, "r%-2d: %08x%s", i, env->regs[i],
                     (i % 4 == 3) ? "\n" : "  ");
    }
}

static ObjectClass *v830_cpu_class_by_name(const char *cpu_model)
{
    g_autofree char *typename = g_strdup_printf(V830_CPU_TYPE_NAME("%s"),
                                                  cpu_model);
    return object_class_by_name(typename);
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps v830_sysemu_ops = {
    .has_work = v830_cpu_has_work,
    .get_phys_addr_debug = v830_cpu_get_phys_addr_debug,
};

static const TCGCPUOps v830_tcg_ops = {
    .guest_default_memory_order = TCG_MO_ALL,
    .mttcg_supported = false,
    .initialize = v830_translate_init,
    .translate_code = v830_translate_code,
    .get_tb_cpu_state = v830_get_tb_cpu_state,
    .synchronize_from_tb = v830_cpu_synchronize_from_tb,
    .restore_state_to_opc = v830_restore_state_to_opc,
    .mmu_index = v830_cpu_mmu_index,
    .tlb_fill = v830_cpu_tlb_fill,
    .pointer_wrap = cpu_pointer_wrap_uint32,
    .cpu_exec_interrupt = v830_cpu_exec_interrupt,
    .cpu_exec_halt = v830_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = v830_cpu_do_interrupt,
};

static void v830_cpu_init(Object *obj)
{
    qdev_init_gpio_in(DEVICE(obj), v830_cpu_set_irq, V830_CPU_IRQ_LINES);
    qdev_init_gpio_in_named(DEVICE(obj), v830_cpu_set_nmi, "nmi", 1);
}

static void v830_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    V830CPUClass *vcc = V830_CPU_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, v830_cpu_realize, &vcc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, v830_cpu_reset_hold, NULL,
                                       &vcc->parent_phases);
    cc->class_by_name = v830_cpu_class_by_name;
    cc->set_pc = v830_cpu_set_pc;
    cc->get_pc = v830_cpu_get_pc;
    cc->dump_state = v830_cpu_dump_state;
    cc->gdb_arch_name = v830_gdb_arch_name;
    cc->gdb_read_register = v830_cpu_gdb_read_register;
    cc->gdb_write_register = v830_cpu_gdb_write_register;
    cc->gdb_core_xml_file = "v830-cpu.xml";
    cc->sysemu_ops = &v830_sysemu_ops;
    cc->tcg_ops = &v830_tcg_ops;
}

#define DEFINE_V830_CPU_TYPE(type_name) \
    { .name = type_name, .parent = TYPE_V830_CPU }

static const TypeInfo v830_cpu_types[] = {
    {
        .name = TYPE_V830_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(V830CPU),
        .instance_align = __alignof(V830CPU),
        .instance_init = v830_cpu_init,
        .abstract = true,
        .class_size = sizeof(V830CPUClass),
        .class_init = v830_cpu_class_init,
    },
    DEFINE_V830_CPU_TYPE("v830-v830-cpu"),
    DEFINE_V830_CPU_TYPE("v831-v830-cpu"),
    DEFINE_V830_CPU_TYPE("v832-v830-cpu"),
    DEFINE_V830_CPU_TYPE("v833-v830-cpu"),
};

DEFINE_TYPES(v830_cpu_types)
