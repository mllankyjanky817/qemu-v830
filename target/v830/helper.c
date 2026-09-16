#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/cpu-ldst.h"

static inline void update_sz(V830CPUState *env, uint32_t result)
{
    env->zf = result; // lazy flags!!!
    env->sf = result;
}

uint32_t helper_saturate(V830CPUState *env, uint64_t result, uint32_t set_flags)
{
    int64_t signed_result = result;

    if (signed_result > INT32_MAX) {
        env->satf = 1;
        if (set_flags) {
            update_sz(env, INT32_MAX);
        }
        return INT32_MAX;
    }
    if (signed_result < INT32_MIN) {
        env->satf = 1;
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

uint32_t helper_add_saturate(V830CPUState *env, uint32_t a, uint32_t b)// modified from ARM op_helper.c
{
    env->ovf = 0;
    uint64_t res = (uint64_t)a + (uint64_t)b;
    env->cyf = res >> 32;

    if ((int32_t)((a ^ res) & ~(a ^ b)) < 0) {
        env->satf = 1;
        env->ovf = -1;
        res = (int32_t)a >> 31 ^ 0x7FFFFFFF;
        env->cyf = res >> 31;
    }

    env->zf = env->sf = (uint32_t)res;
    return (uint32_t)res;
}

uint32_t helper_mac(V830CPUState *env, uint32_t a, uint32_t b, uint32_t c)
{
    int64_t mac = (int64_t)(int32_t)a * (int32_t)b + (int32_t)c;

    if ((int32_t)mac != mac) {
        env->satf = 1;
        return (uint32_t)(0x7FFFFFFF ^ (mac >> 63));
    }

    return (uint32_t)mac;
}

uint32_t helper_mact(V830CPUState *env, uint32_t a, uint32_t b, uint32_t c)
{
    int64_t mac = (int64_t)(int32_t)a * (int32_t)b;
    mac >>= 32;
    mac += (int32_t)c;

    if ((int32_t)mac != mac) {
        env->satf = 1;
        return (uint32_t)(0x7FFFFFFF ^ (mac >> 63));
    }

    return (uint32_t)mac;
}

uint32_t helper_sub_saturate(V830CPUState *env, uint32_t a, uint32_t b)// ditto
{
    env->ovf = 0;
    uint64_t res = (uint64_t)a - (uint64_t)b;
    env->cyf = (res >> 32);

    if ((int32_t)((a ^ b) & (a ^ res)) < 0) {
        env->satf = 1;
        env->ovf = -1;
        res = (int32_t)a >> 31 ^ 0x7FFFFFFF;
        env->cyf = ~(res >> 31);
    }

    env->zf = env->sf = (uint32_t)res;
    return (uint32_t)res;
}

uint32_t helper_mul_saturate(V830CPUState *env, uint32_t a, uint32_t b)// modified from ARM op_helper.c
{
    
    // Perform full 64-bit signed multiplication
    int64_t res = (int64_t)(int32_t)a * (int64_t)(int32_t)b;

    // Overflow occurs if the 64-bit product exceeds the int32_t bounds
    if ((int32_t)res != res) {
        env->satf = 1;
        return (uint32_t)(0x7FFFFFFF ^ (res >> 63));
    }

    return (uint32_t)res;
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

    if (right == 0) {
        helper_divide_error(env);
    }
    if (left == INT32_MIN && right == -1) {// specifically mentioned in v830 manual.
        env->regs[30] = 0;
        env->regs[dst] = INT32_MIN;
        env->ovf = -1;
        update_sz(env, env->regs[dst]);
        return;
    }
    env->regs[30] = left % right;
    env->regs[dst] = left / right;

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

uint32_t helper_shl(V830CPUState *env, uint32_t x, uint32_t i) // From /target/arm/tcg/op_helper.c:1429
{
    int shift = i & 0x1f;
    if (shift >= 32) {
        if (shift == 32)
            env->cyf = x & 1;
        else
            env->cyf = 0;
        return 0;
    } else if (shift != 0) {
        env->cyf = (x >> (32 - shift)) & 1;
        return x << shift;
    }
    return x;
}

uint32_t helper_shr(V830CPUState *env, uint32_t x, uint32_t i) // From /target/arm/tcg/op_helper.c:1445
{
    int shift = i & 0x1f;
    if (shift >= 32) {
        if (shift == 32)
            env->cyf = (x >> 31) & 1;
        else
            env->cyf = 0;
        return 0;
    } else if (shift != 0) {
        env->cyf = (x >> (shift - 1)) & 1;
        return x >> shift;
    }
    return x;
}

uint32_t helper_sar(V830CPUState *env, uint32_t x, uint32_t i) // From /target/arm/tcg/op_helper.c:1461
{
    int shift = i & 0x1f;
    if (shift >= 32) {
        env->cyf = (x >> 31) & 1;
        return (int32_t)x >> 31;
    } else if (shift != 0) {
        env->cyf = (x >> (shift - 1)) & 1;
        return (int32_t)x >> shift;
    }
    return x;
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
        env->psw |= V830_PSW_DP | V830_PSW_NP | V830_PSW_EP |
                    V830_PSW_ID;
        env->pc = 0xffffffe0u;
    } else if (env->psw & V830_PSW_EP) {
        env->fepc = return_pc;
        env->fepsw = v830_psw_read(env);
        env->ecr = (env->ecr & 0xffffu) |
                   (((handler_base + (vector & 0xf)) & 0xffffu) << 16);
        env->psw |= V830_PSW_NP | V830_PSW_ID;
        env->pc = 0xffffffd0u;
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
    V830CPU *cpu = V830_CPU(cs);

    qemu_set_irq(cpu->stopak, 1);
    env->pc += 2;
    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}
