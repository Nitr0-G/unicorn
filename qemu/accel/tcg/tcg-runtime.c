/*
 * Tiny Code Generator for QEMU
 *
 * Copyright (c) 2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/cpu_ldst.h"
#include "exec/exec-all.h"
#include "exec/tb-lookup.h"
#include "hw/core/tcg-cpu-ops.h"
#include "tcg/tcg.h"
#include "tcg/tcg-apple-jit.h"

#include <uc_priv.h>

/* 32-bit helpers */

int32_t HELPER(div_i32)(int32_t arg1, int32_t arg2)
{
    return arg1 / arg2;
}

int32_t HELPER(rem_i32)(int32_t arg1, int32_t arg2)
{
    return arg1 % arg2;
}

uint32_t HELPER(divu_i32)(uint32_t arg1, uint32_t arg2)
{
    return arg1 / arg2;
}

uint32_t HELPER(remu_i32)(uint32_t arg1, uint32_t arg2)
{
    return arg1 % arg2;
}

/* 64-bit helpers */

uint64_t HELPER(shl_i64)(uint64_t arg1, uint64_t arg2)
{
    return arg1 << arg2;
}

uint64_t HELPER(shr_i64)(uint64_t arg1, uint64_t arg2)
{
    return arg1 >> arg2;
}

int64_t HELPER(sar_i64)(int64_t arg1, int64_t arg2)
{
    return arg1 >> arg2;
}

int64_t HELPER(div_i64)(int64_t arg1, int64_t arg2)
{
    return arg1 / arg2;
}

int64_t HELPER(rem_i64)(int64_t arg1, int64_t arg2)
{
    return arg1 % arg2;
}

uint64_t HELPER(divu_i64)(uint64_t arg1, uint64_t arg2)
{
    return arg1 / arg2;
}

uint64_t HELPER(remu_i64)(uint64_t arg1, uint64_t arg2)
{
    return arg1 % arg2;
}

uint64_t HELPER(muluh_i64)(uint64_t arg1, uint64_t arg2)
{
    uint64_t l, h;
    mulu64(&l, &h, arg1, arg2);
    return h;
}

int64_t HELPER(mulsh_i64)(int64_t arg1, int64_t arg2)
{
    uint64_t l, h;
    muls64(&l, &h, arg1, arg2);
    return h;
}

uint32_t HELPER(clz_i32)(uint32_t arg, uint32_t zero_val)
{
    return arg ? clz32(arg) : zero_val;
}

uint32_t HELPER(ctz_i32)(uint32_t arg, uint32_t zero_val)
{
    return arg ? ctz32(arg) : zero_val;
}

uint64_t HELPER(clz_i64)(uint64_t arg, uint64_t zero_val)
{
    return arg ? clz64(arg) : zero_val;
}

uint64_t HELPER(ctz_i64)(uint64_t arg, uint64_t zero_val)
{
    return arg ? ctz64(arg) : zero_val;
}

uint32_t HELPER(clrsb_i32)(uint32_t arg)
{
    return clrsb32(arg);
}

uint64_t HELPER(clrsb_i64)(uint64_t arg)
{
    return clrsb64(arg);
}

uint32_t HELPER(ctpop_i32)(uint32_t arg)
{
    return ctpop32(arg);
}

uint64_t HELPER(ctpop_i64)(uint64_t arg)
{
    return ctpop64(arg);
}

void *HELPER(lookup_tb_ptr)(CPUArchState *env)
{
    CPUState *cpu = env_cpu(env);
    TranslationBlock *tb;
    target_ulong cs_base, pc;
    uint32_t flags;
    struct uc_struct *uc = (struct uc_struct *)cpu->uc;

    tb = tb_lookup__cpu_state(cpu, &pc, &cs_base, &flags,
                              curr_cflags(cpu->uc));
    if (tb == NULL) {
        return uc->tcg_ctx->code_gen_epilogue;
    }
    return tb->tc.ptr;
}

void *HELPER(memset)(void *ptr, int val, void *size)
{
    return memset(ptr, val, (uintptr_t)size);
}

void HELPER(emu_stop)(void *p)
{
    uc_engine *uc = p;

    uc_set_stop_request(uc, true);
    break_translation_loop(uc);
}

void HELPER(exit_atomic)(CPUArchState *env)
{
    cpu_loop_exit_atomic(env_cpu(env), GETPC());
}

void HELPER(uc_tracecode_single)(void *item, uint32_t size, void *handle,
                                 uint64_t address)
{
    struct uc_struct *uc = handle;
    struct list_item *cur = item;
    struct hook *hook;

    if (size == 0) {
        return;
    }
    if (!uc_stop_requested(uc)) {
        for (; cur != NULL && (hook = (struct hook *)cur->data);
             cur = cur->next) {
            if (HOOK_BOUND_CHECK(hook, (uint64_t)address)) {
                JIT_CALLBACK_GUARD(((uc_cb_hookcode_t)hook->callback)(
                    uc, address, size, hook->user_data));
            }
            if (uc_stop_requested(uc)) {
                break;
            }
        }
    }
    if (cpu_loop_exit_requested(uc->cpu)) {
        if (uc->nested_level == 1) {
            tb_exec_unlock(uc);
        }
        qatomic_set(&uc->cpu->tcg_exit_req, 0);
        if (uc->skip_sync_pc_on_exit) {
            cpu_restore_icount(uc->cpu, GETPC());
            uc->skip_sync_pc_on_exit = false;
            cpu_loop_exit(uc->cpu);
        } else {
            cpu_loop_exit_restore(uc->cpu, GETPC());
        }
    }
}

void HELPER(check_exit_request)(void *p, uint32_t in_delay_slot,
                                void *tb_ptr, uint32_t num_insns) {
    uc_engine *uc = p;
    CPUState *cpu = uc->cpu;

    if (cpu_loop_exit_requested(cpu) && !in_delay_slot) {
        // There are stil something we have to before exiting to be compatible with previous behaviors

        // from cpu_tb_exec
        if (uc->nested_level == 1) {
            // Only unlock (allow writing to JIT area) if we are the outmost uc_emu_start
            tb_exec_unlock(uc);
        }
        qatomic_set(&cpu->tcg_exit_req, 0);

        if (uc->skip_sync_pc_on_exit) {
            cpu_restore_icount(cpu, GETPC());
            uc->skip_sync_pc_on_exit = false;
            cpu_loop_exit(cpu);
        } else {
            cpu_loop_exit_restore(cpu, GETPC());
        }
    }

    if (num_insns != 0 && uc->emu_count != 0) {
        size_t remaining = cpu->icount_decr_ptr->u16.low + uc->emu_counter;

        if (remaining >= num_insns) {
            size_t chunk = MIN((size_t)UINT16_MAX, remaining);

            cpu->icount_decr_ptr->u16.low = chunk - num_insns;
            uc->emu_counter = remaining - chunk;
        } else {
            cpu_tcg_synchronize_from_tb(cpu, (TranslationBlock *)tb_ptr);
            cpu->icount_decr_ptr->u16.low = remaining;
            uc->emu_counter = 0;
            if (remaining == 0) {
                uc_set_stop_request(uc, true);
            } else {
                cpu->cflags_next_tb =
                    CF_USE_ICOUNT | (uint32_t)remaining;
                uc->quit_request = true;
            }
            cpu_exit(cpu);
            cpu_loop_exit(cpu);
        }
    }
}
