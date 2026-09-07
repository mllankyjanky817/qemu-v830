#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/cpu-ldst.h"

static void update_sz(V830CPUState *env, uint32_t result)
{
    env->zf = result;
    env->sf = result;
}

uint32_t helper_saturate(V830CPUState *env, uint64_t result, uint32_t set_flags)
{
    int64_t signed_result = result;

    if (signed_result > INT32_MAX) {
        env->psw |= V830_PSW_SAT;
        if (set_flags) {
            update_sz(env, INT32_MAX);
        }
        return INT32_MAX;
    }
    if (signed_result < INT32_MIN) {
        env->psw |= V830_PSW_SAT;
        if (set_flags) {
            update_sz(env, INT32_MIN);
        }
        return INT32_MIN;
    }
    if (set_flags) {
        update_sz(env, signed_result);
    }
    return signed_result;
}

void helper_divide_error(V830CPUState *env)
{
    CPUState *cs = env_cpu(env);
    cs->exception_index = V830_EXCP_DIV0;
    cpu_loop_exit(cs);
}

void helper_div(V830CPUState *env, uint32_t src, uint32_t dst)
{
    int32_t left = env->regs[dst];
    int32_t right = env->regs[src];

    if (right == 0 || (left == INT32_MIN && right == -1)) {
        helper_divide_error(env);
    }
    env->regs[30] = left % right;
    env->regs[dst] = left / right;
    env->ovf = 0;
    update_sz(env, env->regs[dst]);
}

void helper_divu(V830CPUState *env, uint32_t src, uint32_t dst)
{
    uint32_t divisor = env->regs[src];
    uint32_t dividend = env->regs[dst];

    if (divisor == 0) {
        helper_divide_error(env);
    }
    env->regs[30] = dividend % divisor;
    env->regs[dst] = dividend / divisor;
    env->ovf = 0;
    update_sz(env, env->regs[dst]);
}

void helper_ldsr(V830CPUState *env, uint32_t value, uint32_t regid)
{
    switch (regid) {
    case 0: env->eipc = value & ~1u; break;
    case 1: env->eipsw = value & 0x000fdc0fu; break;
    case 2: env->fepc = value & ~1u; break;
    case 3: env->fepsw = value & 0x000fdc0fu; break;
    case 5: v830_psw_write(env, value & 0x000fdc0fu); break;
    case 16: env->dpc = value & ~1u; break;
    case 17: env->dpsw = value & 0x000fdc0fu; break;
    case 31: env->hccw = value; break;
    default: break;
    }
}

uint32_t helper_stsr(V830CPUState *env, uint32_t regid)
{
    switch (regid) {
    case 0: return env->eipc;
    case 1: return env->eipsw;
    case 2: return env->fepc;
    case 3: return env->fepsw;
    case 4: return env->ecr;
    case 5: return v830_psw_read(env);
    case 6: return env->pir;
    case 7: return env->tkcw;
    case 16: return env->dpc;
    case 17: return env->dpsw;
    case 31: return env->hccw;
    default: return 0;
    }
}

uint32_t helper_reti(V830CPUState *env)
{
    if (env->psw & V830_PSW_NP) {
        env->pc = env->fepc;
        v830_psw_write(env, env->fepsw);
    } else {
        env->pc = env->eipc;
        v830_psw_write(env, env->eipsw);
    }
    return env->pc;
}

uint32_t helper_brkret(V830CPUState *env)
{
    env->pc = env->dpc;
    v830_psw_write(env, env->dpsw);
    return env->pc;
}

uint32_t helper_trap(V830CPUState *env, uint32_t vector, uint32_t return_pc)
{
    uint32_t handler_base = 0xffffffa0u + (vector & 0x10);

    if (env->psw & V830_PSW_NP) {
        env->dpc = return_pc;
        env->dpsw = v830_psw_read(env);
        env->psw |= V830_PSW_ID;
        env->pc = 0xffffffe0u;
    } else if (env->psw & V830_PSW_EP) {
        env->fepc = return_pc;
        env->fepsw = v830_psw_read(env);
        env->ecr = (env->ecr & 0xffffu) | (0xffd0u << 16);
        env->psw = (env->psw | V830_PSW_NP | V830_PSW_ID) & ~V830_PSW_EP;
        env->pc = 0xffffffe0u;
    } else {
        env->eipc = return_pc;
        env->eipsw = v830_psw_read(env);
        env->ecr = (env->ecr & 0xffff0000u) |
                   ((handler_base + (vector & 0xf)) & 0xffffu);
        env->psw |= V830_PSW_EP | V830_PSW_ID;
        env->pc = handler_base;
    }
    return env->pc;
}

void helper_raise_illegal_instruction(V830CPUState *env)
{
    CPUState *cs = env_cpu(env);
    cs->exception_index = V830_EXCP_ILLEGAL;
    cpu_loop_exit(cs);
}

void helper_wait(V830CPUState *env)
{
    CPUState *cs = env_cpu(env);

    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}
