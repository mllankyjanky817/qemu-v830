#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translator.h"
#include "exec/translation-block.h"
#include "tcg/tcg-op.h"
#include "tcg/tcg-temp-internal.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    V830CPUState *env;
    TCGv_i32 regs[V830_NUM_GPRS];
    TCGv_i32 psw;
    bool branch;
} DisasContext;

bool decode_insn16(DisasContext *ctx, uint16_t insn);
bool decode_insn32(DisasContext *ctx, uint32_t insn);
#include "decode-insn16.c.inc"
#include "decode-insn32.c.inc"

static TCGv_i32 cpu_pc;
static TCGv_i32 cpu_regs[V830_NUM_GPRS];
static TCGv_i32 cpu_psw;
static TCGv_i32 cpu_ZF, cpu_SF, cpu_OVF, cpu_CYF, cpu_SATF;

void v830_translate_init(void)
{
    int i;

    cpu_pc = tcg_global_mem_new_i32(tcg_env, offsetof(V830CPUState, pc),
                                    "pc");
    for (i = 0; i < V830_NUM_GPRS; i++) {
        char name[8];

        snprintf(name, sizeof(name), "r%d", i);
        cpu_regs[i] = tcg_global_mem_new_i32(
            tcg_env, offsetof(V830CPUState, regs) + i * sizeof(uint32_t),
            name);
    }
    cpu_psw = tcg_global_mem_new_i32(tcg_env, offsetof(V830CPUState, psw),
                                     "psw");
    cpu_ZF = tcg_global_mem_new_i32(tcg_env, offsetof(V830CPUState, zf), "ZF"); //lazy flags
    cpu_SF = tcg_global_mem_new_i32(tcg_env, offsetof(V830CPUState, sf), "SF");
    cpu_OVF = tcg_global_mem_new_i32(tcg_env, offsetof(V830CPUState, ovf), "OVF");
    cpu_CYF = tcg_global_mem_new_i32(tcg_env, offsetof(V830CPUState, cyf), "CYF");
    cpu_SATF = tcg_global_mem_new_i32(tcg_env, offsetof(V830CPUState, satf), "SATF");
}

static void v830_tr_init(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);
    int i;

    ctx->env = cpu_env(cs);
    for (i = 0; i < V830_NUM_GPRS; i++) {
        ctx->regs[i] = cpu_regs[i];
    }
    ctx->psw = cpu_psw;
}

static void v830_tr_tb_start(DisasContextBase *db, CPUState *cs)
{
}

static void v830_tr_insn_start(DisasContextBase *db, CPUState *cs)
{
    tcg_gen_insn_start(db->pc_next, 0, 0);
}

static void v830_update_logic_flags(DisasContext *ctx, TCGv_i32 result)
{
    tcg_gen_mov_i32(cpu_ZF, result);
    tcg_gen_mov_i32(cpu_SF, result);
    tcg_gen_movi_i32(cpu_OVF, 0);
}

static void v830_gen_add(DisasContext *ctx, TCGv_i32 t0, TCGv_i32 t1, TCGv_i32 dest) //modified version of /target/arm/tcg/translate.c:485
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    tcg_gen_movi_i32(tmp, 0);
    tcg_gen_add2_i32(cpu_SF, cpu_CYF, t0, tmp, t1, tmp);
    tcg_gen_mov_i32(cpu_ZF, cpu_SF);
    tcg_gen_xor_i32(cpu_OVF, cpu_SF, t0);
    tcg_gen_xor_i32(tmp, t0, t1);
    tcg_gen_andc_i32(cpu_OVF, cpu_OVF, tmp);
    tcg_gen_mov_i32(dest, cpu_SF);
    tcg_temp_free_i32(tmp);
}

static void v830_gen_sub(DisasContext *ctx, TCGv_i32 t0, TCGv_i32 t1, //modified version of /target/arm/tcg/translate.c:512 
                         TCGv_i32 dest, bool writeback)
{
    TCGv_i32 tmp;
    tcg_gen_sub_i32(cpu_SF, t0, t1);
    tcg_gen_mov_i32(cpu_ZF, cpu_SF);
    tcg_gen_setcond_i32(TCG_COND_LTU, cpu_CYF, t0, t1);
    tcg_gen_xor_i32(cpu_OVF, cpu_SF, t0);
    tmp = tcg_temp_new_i32();
    tcg_gen_xor_i32(tmp, t0, t1);
    tcg_gen_and_i32(cpu_OVF, cpu_OVF, tmp);
    if (writeback) {
        tcg_gen_mov_i32(dest, cpu_SF);
    }
    tcg_temp_free_i32(tmp);
}

static void v830_gen_load(DisasContext *ctx, int src, int dst, int32_t disp,
                          MemOp memop, enum V830MMUIndex mmu_idx)
{
    TCGv_i32 address = tcg_temp_new_i32();

    tcg_gen_addi_i32(address, ctx->regs[src], disp);
    if ((memop & MO_SIZE) == MO_16) {
        tcg_gen_andi_i32(address, address, ~1u);
    } else if ((memop & MO_SIZE) == MO_32) {
        tcg_gen_andi_i32(address, address, ~3u);
    }
    tcg_gen_qemu_ld_i32(ctx->regs[dst], address, mmu_idx, memop);
    tcg_temp_free_i32(address);
}

static void v830_gen_store(DisasContext *ctx, int src, int dst, int32_t disp,
                           MemOp memop, enum V830MMUIndex mmu_idx)
{
    TCGv_i32 address = tcg_temp_new_i32();

    tcg_gen_addi_i32(address, ctx->regs[src], disp);
    if ((memop & MO_SIZE) == MO_16) {
        tcg_gen_andi_i32(address, address, ~1u);
    } else if ((memop & MO_SIZE) == MO_32) {
        tcg_gen_andi_i32(address, address, ~3u);
    }
    tcg_gen_qemu_st_i32(ctx->regs[dst], address, mmu_idx, memop);
    tcg_temp_free_i32(address);
}

static void v830_gen_goto_ptr(DisasContext *ctx)
{
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void v830_gen_goto_tb(DisasContext *ctx, uint32_t dest)
{
    if (translator_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(0);
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, 0);
    } else {
        tcg_gen_movi_i32(cpu_pc, dest);
        v830_gen_goto_ptr(ctx);
        return;
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}

static bool trans_MOV(DisasContext *ctx, arg_MOV *a) // self-explanatory.
{
    tcg_gen_mov_i32(ctx->regs[a->dst], ctx->regs[a->src]);
    return true;
}

static bool trans_ADD(DisasContext *ctx, arg_ADD *a) // uses modified version of TCG for ADDS
{                                                    // (the v800 series updates the flags for almost all instructions)
    v830_gen_add(ctx, ctx->regs[a->dst], ctx->regs[a->src],
                 ctx->regs[a->dst]);
    return true;
}

static bool trans_SUB(DisasContext *ctx, arg_SUB *a) // uses modified version of TCG for SUBS
{
    v830_gen_sub(ctx, ctx->regs[a->dst], ctx->regs[a->src],
                 ctx->regs[a->dst], true);
    return true;
}

static bool trans_CMP(DisasContext *ctx, arg_CMP *a) // the gen is multi-purpose too!
{
    v830_gen_sub(ctx, ctx->regs[a->dst], ctx->regs[a->src],
                 ctx->regs[a->dst], false);
    return true;
}

/* Trans functions for register-register shifts */

static bool trans_SHL(DisasContext *ctx, arg_SHL *a) // Also practically the same as ARM
{
    //v830_gen_shift(ctx, 0, ctx->regs[a->src], ctx->regs[a->dst]);
    gen_helper_shl(ctx->regs[a->dst], tcg_env, ctx->regs[a->dst], ctx->regs[a->src]);
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_SHR(DisasContext *ctx, arg_SHR *a)
{
    //v830_gen_shift(ctx, 1, ctx->regs[a->src], ctx->regs[a->dst]);
    gen_helper_shr(ctx->regs[a->dst], tcg_env, ctx->regs[a->dst], ctx->regs[a->src]);
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_SAR(DisasContext *ctx, arg_SAR *a)
{
    //v830_gen_shift(ctx, 2, ctx->regs[a->src], ctx->regs[a->dst]);
    gen_helper_sar(ctx->regs[a->dst], tcg_env, ctx->regs[a->dst], ctx->regs[a->src]);
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

/* Trans functions for immediate shifts */

static bool trans_SHLI5(DisasContext *ctx, arg_SHLI5 *a)
{
    //v830_gen_shift(ctx, 0, tcg_constant_i32(a->imm), ctx->regs[a->dst]);
    gen_helper_shl(ctx->regs[a->dst], tcg_env, ctx->regs[a->dst], tcg_constant_i32(a->imm));
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_SHRI5(DisasContext *ctx, arg_SHRI5 *a)
{
    //v830_gen_shift(ctx, 1, tcg_constant_i32(a->imm), ctx->regs[a->dst]);
    gen_helper_shr(ctx->regs[a->dst], tcg_env, ctx->regs[a->dst], tcg_constant_i32(a->imm));
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_SARI5(DisasContext *ctx, arg_SARI5 *a)
{
    //v830_gen_shift(ctx, 2, tcg_constant_i32(a->imm), ctx->regs[a->dst]);
    gen_helper_sar(ctx->regs[a->dst], tcg_env, ctx->regs[a->dst], tcg_constant_i32(a->imm));
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_JMP(DisasContext *ctx, arg_JMP *a)
{
    tcg_gen_mov_i32(cpu_pc, ctx->regs[a->src]);
    v830_gen_goto_ptr(ctx);
    return true;
}

static void v830_gen_mul(DisasContext *ctx, int src_idx, int dst_idx, bool is_signed) 
// Can be represented in tcg since flags don't require conditional branching that would clobber temporaries.
{
    TCGv_i32 t0 = ctx->regs[src_idx];
    TCGv_i32 t1 = ctx->regs[dst_idx];

    /* Destination registers */
    TCGv_i32 high = ctx->regs[30];
    /* If dst is r30, use a temporary to prevent low and high from aliasing */
    TCGv_i32 low = (dst_idx == 30) ? tcg_temp_new_i32() : ctx->regs[dst_idx];

    if (is_signed) {
        tcg_gen_muls2_i32(low, high, t0, t1);

        /* Overflow detection: high != (low >> 31) */
        tcg_gen_sari_i32(cpu_OVF, low, 31);
        tcg_gen_xor_i32(cpu_OVF, cpu_OVF, high);
        tcg_gen_setcondi_i32(TCG_COND_NE, cpu_OVF, cpu_OVF, 0);
        tcg_gen_shli_i32(cpu_OVF, cpu_OVF, 31);
    } else {
        tcg_gen_mulu2_i32(low, high, t0, t1);

        /* Unsigned overflow detection: high != 0 */
        tcg_gen_setcondi_i32(TCG_COND_NE, cpu_OVF, high, 0);
        tcg_gen_shli_i32(cpu_OVF, cpu_OVF, 31);
    }

    /* If dst is r30, low-order bits must overwrite r30. possibly redundant. */
    if (dst_idx == 30) {
        tcg_gen_mov_i32(ctx->regs[30], low);
    }

    /* Flags set based on low 32 bits */
    tcg_gen_mov_i32(cpu_ZF, low);
    tcg_gen_mov_i32(cpu_SF, low);
}

static bool trans_MUL(DisasContext *ctx, arg_MUL *a)
{
    v830_gen_mul(ctx, a->src, a->dst, true);

    return true;
}

static bool trans_MULU(DisasContext *ctx, arg_MULU *a)
{
    v830_gen_mul(ctx, a->src, a->dst, false);
    return true;
}

static bool trans_DIV(DisasContext *ctx, arg_DIV *a) // needs to be in helpers because of div by zero and signed overflow handling.
{
    gen_helper_div(tcg_env, tcg_constant_i32(a->src),
                   tcg_constant_i32(a->dst));
    return true;
}

static bool trans_DIVU(DisasContext *ctx, arg_DIVU *a) // needs to be in helpers because of div by zero handling.
{
    gen_helper_divu(tcg_env, tcg_constant_i32(a->src),
                    tcg_constant_i32(a->dst));
    return true;
}

static bool trans_OR(DisasContext *ctx, arg_OR *a)
{
    tcg_gen_or_i32(ctx->regs[a->dst], ctx->regs[a->dst], ctx->regs[a->src]);
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_AND(DisasContext *ctx, arg_AND *a)
{
    tcg_gen_and_i32(ctx->regs[a->dst], ctx->regs[a->dst], ctx->regs[a->src]);
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_XOR(DisasContext *ctx, arg_XOR *a)
{
    tcg_gen_xor_i32(ctx->regs[a->dst], ctx->regs[a->dst], ctx->regs[a->src]);
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_NOT(DisasContext *ctx, arg_NOT *a)
{
    tcg_gen_not_i32(ctx->regs[a->dst], ctx->regs[a->src]);
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_MOVI(DisasContext *ctx, arg_MOVI *a)
{
    tcg_gen_movi_i32(ctx->regs[a->dst], a->imm);
    return true;
}

static bool trans_ADDI5(DisasContext *ctx, arg_ADDI5 *a)
{
    v830_gen_add(ctx, ctx->regs[a->dst], tcg_constant_i32(a->imm),
                 ctx->regs[a->dst]);
    return true;
}

/* Inline condition evaluation to avoid helper call overhead */
static inline TCGv_i32 v830_gen_condition(DisasContext *ctx, int cond)
{
    TCGv_i32 result = tcg_temp_new_i32();
    TCGv_i32 cy = tcg_temp_new_i32();
    TCGv_i32 z = tcg_temp_new_i32();
    TCGv_i32 temp = tcg_temp_new_i32();
    
    /* Convert lazy flags to booleans only at this condition boundary. */
    switch (cond & 0xf) {
    case 0x0: tcg_gen_setcondi_i32(TCG_COND_LT, result, cpu_OVF, 0); break;
    case 0x1: tcg_gen_setcondi_i32(TCG_COND_NE, result, cpu_CYF, 0); break;
    case 0x2: tcg_gen_setcondi_i32(TCG_COND_EQ, result, cpu_ZF, 0); break;
    case 0x3:
        tcg_gen_setcondi_i32(TCG_COND_NE, cy, cpu_CYF, 0);
        tcg_gen_setcondi_i32(TCG_COND_EQ, z, cpu_ZF, 0);
        tcg_gen_or_i32(result, cy, z);
        break;
    case 0x4: tcg_gen_setcondi_i32(TCG_COND_LT, result, cpu_SF, 0); break;
    case 0x5: tcg_gen_movi_i32(result, 1); break;
    case 0x6:
        tcg_gen_xor_i32(temp, cpu_SF, cpu_OVF);
        tcg_gen_setcondi_i32(TCG_COND_LT, result, temp, 0);
        break;
    case 0x7:
        tcg_gen_xor_i32(temp, cpu_SF, cpu_OVF);
        tcg_gen_setcondi_i32(TCG_COND_LT, result, temp, 0);
        tcg_gen_setcondi_i32(TCG_COND_EQ, z, cpu_ZF, 0);
        tcg_gen_or_i32(result, result, z);
        break;
    case 0x8: tcg_gen_setcondi_i32(TCG_COND_GE, result, cpu_OVF, 0); break;
    case 0x9: tcg_gen_setcondi_i32(TCG_COND_EQ, result, cpu_CYF, 0); break;
    case 0xa: tcg_gen_setcondi_i32(TCG_COND_NE, result, cpu_ZF, 0); break;
    case 0xb:
        tcg_gen_setcondi_i32(TCG_COND_EQ, cy, cpu_CYF, 0);
        tcg_gen_setcondi_i32(TCG_COND_NE, z, cpu_ZF, 0);
        tcg_gen_and_i32(result, cy, z);
        break;
    case 0xc: tcg_gen_setcondi_i32(TCG_COND_GE, result, cpu_SF, 0); break;
    case 0xd: tcg_gen_movi_i32(result, 0); break;
    case 0xe:
        tcg_gen_xor_i32(temp, cpu_SF, cpu_OVF);
        tcg_gen_setcondi_i32(TCG_COND_GE, result, temp, 0);
        break;
    case 0xf:
        tcg_gen_xor_i32(temp, cpu_SF, cpu_OVF);
        tcg_gen_setcondi_i32(TCG_COND_GE, result, temp, 0);
        tcg_gen_setcondi_i32(TCG_COND_NE, z, cpu_ZF, 0);
        tcg_gen_and_i32(result, result, z);
        break;
    }
    tcg_temp_free_i32(cy);
    tcg_temp_free_i32(z);
    tcg_temp_free_i32(temp);
    return result;
}

static bool trans_SETF(DisasContext *ctx, arg_SETF *a)
{
    TCGv_i32 condition = v830_gen_condition(ctx, a->imm);
    tcg_gen_mov_i32(ctx->regs[a->dst], condition);
    tcg_temp_free_i32(condition);
    return true;
}

static bool trans_CMPI5(DisasContext *ctx, arg_CMPI5 *a)
{
    v830_gen_sub(ctx, ctx->regs[a->dst], tcg_constant_i32(a->imm),
                 ctx->regs[a->dst], false);
    return true;
}

static bool trans_BCOND(DisasContext *ctx, arg_BCOND *a)
{
    int32_t displacement = (((int32_t)a->disp << 1) | a->advanced) & ~1;
    TCGv_i32 condition = v830_gen_condition(ctx, a->cond);

    tcg_gen_movcond_i32(TCG_COND_NE, cpu_pc, condition,
                        tcg_constant_i32(0),
                        tcg_constant_i32(ctx->base.pc_next - 2 + displacement),
                        tcg_constant_i32(ctx->base.pc_next));
    tcg_temp_free_i32(condition);
    v830_gen_goto_ptr(ctx);
    return true;
}

static bool trans_ABCOND(DisasContext *ctx, arg_ABCOND *a)
{
    int32_t displacement = (((int32_t)a->disp << 1) | a->advanced) & ~1;
    TCGv_i32 condition = v830_gen_condition(ctx, a->cond);

    tcg_gen_movcond_i32(TCG_COND_NE, cpu_pc, condition,
                        tcg_constant_i32(0),
                        tcg_constant_i32(ctx->base.pc_next - 2 + displacement),
                        tcg_constant_i32(ctx->base.pc_next));
    tcg_temp_free_i32(condition);
    v830_gen_goto_ptr(ctx);
    return true;
}

static bool trans_JR(DisasContext *ctx, arg_JR *a)
{
    uint32_t insn_pc = ctx->base.pc_next - 4;

    v830_gen_goto_tb(ctx, insn_pc + a->disp);
    return true;
}

static bool trans_JAL(DisasContext *ctx, arg_JAL *a)
{
    uint32_t insn_pc = ctx->base.pc_next - 4;

    tcg_gen_movi_i32(ctx->regs[31], insn_pc + 4);
    v830_gen_goto_tb(ctx, insn_pc + a->disp);
    return true;
}

static bool trans_LD_B(DisasContext *ctx, arg_LD_B *a)
{
    v830_gen_load(ctx, a->src, a->dst, a->imm, MO_SB, V830_MMU_DATA);
    return true;
}

static bool trans_LD_H(DisasContext *ctx, arg_LD_H *a)
{
    v830_gen_load(ctx, a->src, a->dst, a->imm, MO_SW, V830_MMU_DATA);
    return true;
}

static bool trans_LD_W(DisasContext *ctx, arg_LD_W *a)
{
    v830_gen_load(ctx, a->src, a->dst, a->imm, MO_UL, V830_MMU_DATA);
    return true;
}

static bool trans_ST_B(DisasContext *ctx, arg_ST_B *a)
{
    v830_gen_store(ctx, a->src, a->dst, a->imm, MO_UB, V830_MMU_DATA);
    return true;
}

static bool trans_ST_H(DisasContext *ctx, arg_ST_H *a)
{
    v830_gen_store(ctx, a->src, a->dst, a->imm, MO_UW, V830_MMU_DATA);
    return true;
}

static bool trans_ST_W(DisasContext *ctx, arg_ST_W *a)
{
    v830_gen_store(ctx, a->src, a->dst, a->imm, MO_UL, V830_MMU_DATA);
    return true;
}

static bool trans_IN_B(DisasContext *ctx, arg_IN_B *a)
{
    v830_gen_load(ctx, a->src, a->dst, a->imm, MO_UB, V830_MMU_IO);
    return true;
}

static bool trans_IN_H(DisasContext *ctx, arg_IN_H *a)
{
    v830_gen_load(ctx, a->src, a->dst, a->imm, MO_UW, V830_MMU_IO);
    return true;
}

static bool trans_IN_W(DisasContext *ctx, arg_IN_W *a)
{
    v830_gen_load(ctx, a->src, a->dst, a->imm, MO_UL, V830_MMU_IO);
    return true;
}

static bool trans_OUT_B(DisasContext *ctx, arg_OUT_B *a)
{
    v830_gen_store(ctx, a->src, a->dst, a->imm, MO_UB, V830_MMU_IO);
    return true;
}

static bool trans_OUT_H(DisasContext *ctx, arg_OUT_H *a)
{
    v830_gen_store(ctx, a->src, a->dst, a->imm, MO_UW, V830_MMU_IO);
    return true;
}

static bool trans_OUT_W(DisasContext *ctx, arg_OUT_W *a)
{
    v830_gen_store(ctx, a->src, a->dst, a->imm, MO_UL, V830_MMU_IO);
    return true;
}

static bool trans_MOVEA(DisasContext *ctx, arg_MOVEA *a)
{
    tcg_gen_addi_i32(ctx->regs[a->dst], ctx->regs[a->src], (int16_t)a->imm);
    return true;
}

static bool trans_ADDI(DisasContext *ctx, arg_ADDI *a)
{
    v830_gen_add(ctx, ctx->regs[a->src], tcg_constant_i32((int16_t)a->imm),
                 ctx->regs[a->dst]);
    return true;
}

static bool trans_ORI(DisasContext *ctx, arg_ORI *a)
{
    tcg_gen_ori_i32(ctx->regs[a->dst], ctx->regs[a->src], a->imm);
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_ANDI(DisasContext *ctx, arg_ANDI *a)
{
    tcg_gen_andi_i32(ctx->regs[a->dst], ctx->regs[a->src], a->imm);
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    tcg_gen_movi_i32(cpu_SF, 0);
    return true;
}

static bool trans_XORI(DisasContext *ctx, arg_XORI *a)
{
    tcg_gen_xori_i32(ctx->regs[a->dst], ctx->regs[a->src], a->imm);
    v830_update_logic_flags(ctx, ctx->regs[a->dst]);
    return true;
}

static bool trans_MOVHI(DisasContext *ctx, arg_MOVHI *a)
{
    tcg_gen_addi_i32(ctx->regs[a->dst], ctx->regs[a->src], a->imm << 16);
    return true;
}

static TCGv_i32 v830_gen_saturate_i64(DisasContext *ctx, TCGv_i64 res64)
{
    TCGv_i32 res32 = tcg_temp_new_i32();
    TCGv_i64 clamped = tcg_temp_new_i64();
    TCGv_i64 above_max = tcg_temp_new_i64();
    TCGv_i64 below_min = tcg_temp_new_i64();
    TCGv_i64 out_of_range = tcg_temp_new_i64();
    TCGv_i32 sat_mask = tcg_temp_new_i32();

    tcg_gen_setcondi_i64(TCG_COND_GT, above_max, res64, INT32_MAX);
    tcg_gen_setcondi_i64(TCG_COND_LT, below_min, res64, INT32_MIN);
    tcg_gen_or_i64(out_of_range, above_max, below_min);
    tcg_gen_extrl_i64_i32(sat_mask, out_of_range);
    tcg_gen_or_i32(cpu_SATF, cpu_SATF, sat_mask);

    /* Clamp 64-bit result to signed 32-bit limits: [INT32_MIN, INT32_MAX] */
    tcg_gen_smin_i64(clamped, res64, tcg_constant_i64(INT32_MAX));
    tcg_gen_smax_i64(clamped, clamped, tcg_constant_i64(INT32_MIN));
    tcg_gen_extrl_i64_i32(res32, clamped);

    tcg_temp_free_i64(clamped);
    tcg_temp_free_i64(above_max);
    tcg_temp_free_i64(below_min);
    tcg_temp_free_i64(out_of_range);
    tcg_temp_free_i32(sat_mask);

    return res32;
}

static bool trans_MULI(DisasContext *ctx, arg_MULI *a)
{
    TCGv_i64 src1_64 = tcg_temp_new_i64();
    TCGv_i64 imm_64 = tcg_temp_new_i64();
    TCGv_i64 res64 = tcg_temp_new_i64();

    /* Sign-extend 32-bit src register and 16-bit immediate to 64-bit */
    tcg_gen_ext_i32_i64(src1_64, ctx->regs[a->src]);
    tcg_gen_movi_i64(imm_64, (int16_t)a->imm);

    /* res64 = (int64_t)(int32_t)regs[src] * (int16_t)imm */
    tcg_gen_mul_i64(res64, src1_64, imm_64);

    /* Clamp and write back without modifying flags (set_flags = 0) */
    TCGv_i32 res32 = v830_gen_saturate_i64(ctx, res64);
    tcg_gen_mov_i32(ctx->regs[a->dst], res32);
    tcg_temp_free_i32(res32);
    tcg_temp_free_i64(src1_64);
    tcg_temp_free_i64(imm_64);
    tcg_temp_free_i64(res64);
    return true;
}

static bool trans_MACI(DisasContext *ctx, arg_MACI *a)
{

    gen_helper_mac(ctx->regs[a->dst], tcg_env, ctx->regs[a->src], tcg_constant_i32(a->imm), ctx->regs[a->dst]);
    return true;
}

static bool trans_CAXI(DisasContext *ctx, arg_CAXI *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 old_val = tcg_temp_new_i32();
    TCGv_i32 cmp_val = tcg_temp_new_i32();
    TCGv_i32 new_val = tcg_temp_new_i32();

    /* 1. Calculate word-aligned memory address: (regs[src] + imm) & ~3 */
    tcg_gen_addi_i32(addr, ctx->regs[a->src], (int16_t)a->imm);
    tcg_gen_andi_i32(addr, addr, ~3u);

    /* 2. Set up compare (dst) and store (r30) values */
    tcg_gen_mov_i32(cmp_val, ctx->regs[a->dst]);
    tcg_gen_mov_i32(new_val, ctx->regs[30]);

    /* 3. Execute atomic compare-and-swap via softmmu */
    tcg_gen_atomic_cmpxchg_i32(old_val, addr, cmp_val, new_val,
                              0, MO_TEUL);

    /* 4. Update arithmetic flags for subtraction: dst - old_val */
    v830_gen_sub(ctx, ctx->regs[a->dst], old_val, NULL, false);

    /* 5. Return the previous memory value into dst */
    tcg_gen_mov_i32(ctx->regs[a->dst], old_val);

    tcg_temp_free_i32(addr);
    tcg_temp_free_i32(old_val);
    tcg_temp_free_i32(cmp_val);
    tcg_temp_free_i32(new_val);
    return true;
}

static bool trans_SATADD3(DisasContext *ctx, arg_SATADD3 *a)
{
    gen_helper_add_saturate(ctx->regs[a->r3], tcg_env, ctx->regs[a->r1], ctx->regs[a->r2]);
    //v830_gen_sat_addsub3(ctx, a->r1, a->r2, a->r3, false);
    return true;
}

static bool trans_SATSUB3(DisasContext *ctx, arg_SATSUB3 *a)
{
    gen_helper_sub_saturate(ctx->regs[a->r3], tcg_env, ctx->regs[a->r2], ctx->regs[a->r1]);
    //v830_gen_sat_addsub3(ctx, a->r1, a->r2, a->r3, true);
    return true;
}

static bool trans_MIN3(DisasContext *ctx, arg_MIN3 *a)
{
    tcg_gen_movcond_i32(TCG_COND_LT, ctx->regs[a->r3],
                        ctx->regs[a->r1], ctx->regs[a->r2],
                        ctx->regs[a->r1], ctx->regs[a->r2]);
    return true;
}

static bool trans_MAX3(DisasContext *ctx, arg_MAX3 *a)
{
    tcg_gen_movcond_i32(TCG_COND_GT, ctx->regs[a->r3],
                        ctx->regs[a->r1], ctx->regs[a->r2],
                        ctx->regs[a->r1], ctx->regs[a->r2]);
    return true;
}

static bool trans_SHLD3(DisasContext *ctx, arg_SHRD3 *a)
{
    TCGv_i64 pair = tcg_temp_new_i64();
    TCGv_i64 shift = tcg_temp_new_i64();// this needs to be 64 bits because extrh requires a 64-bit shift value.

    // combine 32-bit r3 (high) and r2 (low) into a 64-bit pair
    tcg_gen_concat_i32_i64(pair, ctx->regs[a->r2], ctx->regs[a->r3]);
    
    // mask shift count to 5 bits
    tcg_gen_extu_i32_i64(shift, ctx->regs[a->r1]);
    tcg_gen_andi_i64(shift, shift, 0x1f);

    // shift left and store high 32 bits straight to dest
    tcg_gen_shl_i64(pair, pair, shift);
    tcg_gen_extrh_i64_i32(ctx->regs[a->r3], pair);

    return true;
}

static bool trans_SHRD3(DisasContext *ctx, arg_SHRD3 *a)
{
    TCGv_i64 pair = tcg_temp_new_i64();
    TCGv_i64 shift = tcg_temp_new_i64();// same thing

    // combine 32-bit r3 (high) and r2 (low) into a 64-bit pair
    tcg_gen_concat_i32_i64(pair, ctx->regs[a->r2], ctx->regs[a->r3]);
    
    // mask shift count to 5 bits
    tcg_gen_extu_i32_i64(shift, ctx->regs[a->r1]);
    tcg_gen_andi_i64(shift, shift, 0x1f);

    // shift right and store low 32 bits straight to dest
    tcg_gen_shr_i64(pair, pair, shift);
    tcg_gen_extrl_i64_i32(ctx->regs[a->r3], pair);

    return true;
}

static bool trans_MACT3(DisasContext *ctx, arg_MACT3 *a)
{
    gen_helper_mact(ctx->regs[a->r3], tcg_env, ctx->regs[a->r1], ctx->regs[a->r2], ctx->regs[a->r3]);
    return true;
}

static bool trans_MAC3(DisasContext *ctx, arg_MAC3 *a)

{
    gen_helper_mac(ctx->regs[a->r3], tcg_env, ctx->regs[a->r1], ctx->regs[a->r2], ctx->regs[a->r3]);
    //v830_gen_ext_mul_inlined(ctx, a->r1, a->r2, a->r3, true, false);
    return true;
}

static bool trans_MULT3(DisasContext *ctx, arg_MULT3 *a)
{
    TCGv_i32 discard = tcg_temp_new_i32(); // you NEED a dummy variable; you can't use NULL for something you don't need.
    tcg_gen_muls2_i32(discard, ctx->regs[a->r3], ctx->regs[a->r1], ctx->regs[a->r2]); 
    tcg_temp_free_i32(discard);
    return true;
}

static bool trans_MUL3(DisasContext *ctx, arg_MUL3 *a)
{
    gen_helper_mul_saturate (ctx->regs[a->r3], tcg_env, ctx->regs[a->r2], ctx->regs[a->r1]);
    //v830_gen_ext_mul_inlined(ctx, a->r1, a->r2, a->r3, false, false);
    return true;
}

static void v830_gen_block_transfer(DisasContext *ctx, int source, int target,
                                    bool to_external, bool instruction_ram)
{
    TCGv_i32 internal = tcg_temp_new_i32();
    tcg_gen_addi_i32(internal, ctx->regs[target], instruction_ram ? (int32_t)0xfe000000 : 0);
    TCGv_i32 src_base = to_external ? internal : ctx->regs[source];// better to eval this outside of the loop
    TCGv_i32 dst_base = to_external ? ctx->regs[source] : internal;// ditto
    for (unsigned offset = 0; offset < 16; offset += 4) {
        TCGv_i32 addr_src = tcg_temp_new_i32();
        TCGv_i32 addr_dst = tcg_temp_new_i32();
        TCGv_i32 val = tcg_temp_new_i32();

        tcg_gen_addi_i32(addr_src, src_base, offset);
        tcg_gen_qemu_ld_i32(val, addr_src, V830_MMU_INTERNAL, MO_UL);

        tcg_gen_addi_i32(addr_dst, dst_base, offset);
        tcg_gen_qemu_st_i32(val, addr_dst, V830_MMU_INTERNAL, MO_UL);
    }
}

static bool trans_BILD(DisasContext *ctx, arg_BILD *a)
{
    v830_gen_block_transfer(ctx, a->r1, a->r2, false, true);
    return true;
}

static bool trans_BDLD(DisasContext *ctx, arg_BDLD *a)
{
    v830_gen_block_transfer(ctx, a->r1, a->r2, false, false);
    return true;
}

static bool trans_BIST(DisasContext *ctx, arg_BIST *a)
{
    v830_gen_block_transfer(ctx, a->r1, a->r2, true, true);
    return true;
}

static bool trans_BDST(DisasContext *ctx, arg_BDST *a)
{
    v830_gen_block_transfer(ctx, a->r1, a->r2, true, false);
    return true;
}

static bool trans_TRAP(DisasContext *ctx, arg_TRAP *a)
{
    TCGv_i32 target = tcg_temp_new_i32();

    gen_helper_trap(target, tcg_env, tcg_constant_i32(a->imm),
                    tcg_constant_i32(ctx->base.pc_next));
    tcg_gen_mov_i32(cpu_pc, target);
    tcg_temp_free_i32(target);
    v830_gen_goto_ptr(ctx);
    return true;
}

static bool trans_EI(DisasContext *ctx, arg_EI *a)
{
    tcg_gen_andi_i32(ctx->psw, ctx->psw, ~V830_PSW_ID);
    return true;
}

static bool trans_DI(DisasContext *ctx, arg_DI *a)
{
    tcg_gen_ori_i32(ctx->psw, ctx->psw, V830_PSW_ID);
    return true;
}

static bool trans_RETI(DisasContext *ctx, arg_RETI *a)
{
    TCGv_i32 target = tcg_temp_new_i32();

    gen_helper_reti(target, tcg_env);
    tcg_gen_mov_i32(cpu_pc, target);
    tcg_temp_free_i32(target);
    v830_gen_goto_ptr(ctx);
    return true;
}

static bool trans_BRKRET(DisasContext *ctx, arg_BRKRET *a)
{
    TCGv_i32 target = tcg_temp_new_i32();

    gen_helper_brkret(target, tcg_env);
    tcg_gen_mov_i32(cpu_pc, target);
    tcg_temp_free_i32(target);
    v830_gen_goto_ptr(ctx);
    return true;
}

static bool trans_HALT(DisasContext *ctx, arg_HALT *a)
{
    gen_helper_wait(tcg_env);
    ctx->branch = true;
    return true;
}

static bool trans_STBY(DisasContext *ctx, arg_STBY *a)
{
    gen_helper_wait(tcg_env);
    ctx->branch = true;
    return true;
}

static bool trans_LDSR(DisasContext *ctx, arg_LDSR *a)
{
    gen_helper_ldsr(tcg_env, ctx->regs[a->src],
                    tcg_constant_i32(a->dst));
    return true;
}

static bool trans_STSR(DisasContext *ctx, arg_STSR *a)
{
    gen_helper_stsr(ctx->regs[a->src], tcg_env,
                    tcg_constant_i32(a->dst));
    return true;
}

static void v830_tr_translate_insn(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);
    uint16_t first = translator_lduw(ctx->env, db, db->pc_next);
    uint32_t insn = first;

    db->pc_next += 2;
    if ((first & 0xe000) >= 0xa000) {
        insn = ((uint32_t)first << 16) |
               translator_lduw(ctx->env, db, db->pc_next);
        db->pc_next += 2;
        if (!decode_insn32(ctx, insn)) {
            gen_helper_raise_illegal_instruction(tcg_env);
        }
    } else if (!decode_insn16(ctx, insn)) {
        gen_helper_raise_illegal_instruction(tcg_env);
    }
    tcg_gen_movi_i32(ctx->regs[0], 0);
}

static void v830_tr_tb_stop(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);

    if (db->is_jmp == DISAS_NORETURN) {
        return;
    }
    if (!ctx->branch) {
        v830_gen_goto_tb(ctx, db->pc_next);
        return;
    }
    tcg_gen_exit_tb(NULL, 0);
}

static const TranslatorOps v830_tr_ops = {
    .init_disas_context = v830_tr_init,
    .tb_start = v830_tr_tb_start,
    .insn_start = v830_tr_insn_start,
    .translate_insn = v830_tr_translate_insn,
    .tb_stop = v830_tr_tb_stop,
};

void v830_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext ctx = { };
    translator_loop(cs, tb, max_insns, pc, host_pc, &v830_tr_ops,
                    &ctx.base, TCG_TYPE_I32);
}
