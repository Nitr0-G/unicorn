#ifndef GEN_ICOUNT_H
#define GEN_ICOUNT_H

#include "qemu/timer.h"

/* Helpers for instruction counting code generation.  */

static inline void gen_io_start(TCGContext *tcg_ctx)
{
    TCGv_i32 tmp = tcg_const_i32(tcg_ctx, 1);
    tcg_gen_st_i32(tcg_ctx, tmp, tcg_ctx->cpu_env,
                   offsetof(ArchCPU, parent_obj.can_do_io) -
                   offsetof(ArchCPU, env));
    tcg_temp_free_i32(tcg_ctx, tmp);
}

/*
 * cpu->can_do_io is cleared automatically at the beginning of
 * each translation block.  The cost is minimal and only paid
 * for -icount, plus it would be very easy to forget doing it
 * in the translator.  Therefore, backends only need to call
 * gen_io_start.
 */
static inline void gen_io_end(TCGContext *tcg_ctx)
{
    TCGv_i32 tmp = tcg_const_i32(tcg_ctx, 0);
    tcg_gen_st_i32(tcg_ctx, tmp, tcg_ctx->cpu_env,
                   offsetof(ArchCPU, parent_obj.can_do_io) -
                   offsetof(ArchCPU, env));
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static inline void gen_tb_start(TCGContext *tcg_ctx, TranslationBlock *tb)
{
    if (tb_cflags(tb) & CF_USE_ICOUNT) {
        TCGv_ptr puc = tcg_const_ptr(tcg_ctx, tcg_ctx->uc);
        TCGv_ptr ttb = tcg_const_ptr(tcg_ctx, tb);
        TCGv_i32 tmp = tcg_const_i32(tcg_ctx, 0);
        TCGv_i32 insns = tcg_temp_new_i32(tcg_ctx);

        if (tcg_ctx->delay_slot_flag != NULL) {
            tcg_gen_mov_i32(tcg_ctx, tmp, tcg_ctx->delay_slot_flag);
        }
        tcg_gen_movi_i32(tcg_ctx, insns, 0xdeadbeef);
        tcg_ctx->icount_start_insn = tcg_last_op(tcg_ctx);
        gen_helper_check_counted_entry(tcg_ctx, puc, tmp, ttb, insns);
        tcg_temp_free_i32(tcg_ctx, insns);
        tcg_temp_free_i32(tcg_ctx, tmp);
        tcg_temp_free_ptr(tcg_ctx, ttb);
        tcg_temp_free_ptr(tcg_ctx, puc);
    } else {
        TCGv_i32 exit_req = tcg_temp_new_i32(tcg_ctx);
        TCGLabel *no_exit = gen_new_label(tcg_ctx);

        tcg_gen_ld_i32(tcg_ctx, exit_req, tcg_ctx->cpu_env,
                       offsetof(ArchCPU, parent_obj.exit_request_pending) -
                           offsetof(ArchCPU, env));
        tcg_gen_brcondi_i32(tcg_ctx, TCG_COND_EQ, exit_req, 0, no_exit);
        {
            TCGv_ptr puc = tcg_const_ptr(tcg_ctx, tcg_ctx->uc);
            TCGv_i32 tmp = tcg_const_i32(tcg_ctx, 0);

            gen_helper_exit_request(tcg_ctx, puc, tmp);
            tcg_temp_free_i32(tcg_ctx, tmp);
            tcg_temp_free_ptr(tcg_ctx, puc);
        }
        gen_set_label(tcg_ctx, no_exit);
        tcg_temp_free_i32(tcg_ctx, exit_req);
    }
}

static inline void gen_tb_end(TCGContext *tcg_ctx, TranslationBlock *tb, int num_insns)
{
    if (tcg_ctx->delay_slot_flag != NULL){
        tcg_temp_free_i32(tcg_ctx, tcg_ctx->delay_slot_flag);
    }
    tcg_ctx->delay_slot_flag = NULL;
    if (tb_cflags(tb) & CF_USE_ICOUNT) {
        /* Update the num_insn immediate parameter now that we know
         * the actual insn count.  */
        tcg_set_insn_param(tcg_ctx->icount_start_insn, 1, num_insns);
    }

    tcg_gen_exit_tb(tcg_ctx, tb, TB_EXIT_REQUESTED);
}

#endif
