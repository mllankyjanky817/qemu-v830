#include "qemu/osdep.h"
#include "cpu.h"
#include "gdbstub/helpers.h"

int v830_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int reg)
{
    V830CPUState *env = cpu_env(cs);
    if (reg < V830_NUM_GPRS) {
        return gdb_get_reg32(buf, env->regs[reg]);
    }
    switch (reg) {
    case 32: return gdb_get_reg32(buf, env->eipc);
    case 33: return gdb_get_reg32(buf, env->eipsw);
    case 34: return gdb_get_reg32(buf, env->fepc);
    case 35: return gdb_get_reg32(buf, env->fepsw);
    case 36: return gdb_get_reg32(buf, env->ecr);
    case 37: return gdb_get_reg32(buf, v830_psw_read(env));
    case 38: return gdb_get_reg32(buf, env->pir);
    case 39: return gdb_get_reg32(buf, env->tkcw);
    case 40: return gdb_get_reg32(buf, env->dpc);
    case 41: return gdb_get_reg32(buf, env->dpsw);
    case 42: return gdb_get_reg32(buf, env->hccw);
    case 43: return gdb_get_reg32(buf, env->pc);
    default: return 0;
    }
}

int v830_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int reg)
{
    V830CPUState *env = cpu_env(cs);
    uint32_t value = ldl_p(buf);
    if (reg < V830_NUM_GPRS) {
        env->regs[reg] = value;
    } else {
        switch (reg) {
        case 32: env->eipc = value; break;
        case 33: env->eipsw = value; break;
        case 34: env->fepc = value; break;
        case 35: env->fepsw = value; break;
        case 36: env->ecr = value; break;
        case 37: v830_psw_write(env, value); break;
        case 38: env->pir = value; break;
        case 39: env->tkcw = value; break;
        case 40: env->dpc = value; break;
        case 41: env->dpsw = value; break;
        case 42: env->hccw = value; break;
        case 43: env->pc = value; break;
        default: return 0;
        }
    }
    return 4;
}
