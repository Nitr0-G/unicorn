/*
 * Active translation block execution tracking.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef ACCEL_TCG_TB_EXEC_FRAME_H
#define ACCEL_TCG_TB_EXEC_FRAME_H

static inline void tb_exec_frame_set_tb(UcTbExecFrame *frame,
                                        TranslationBlock *tb)
{
    frame->tb = tb;
}

static inline void tb_exec_frame_clear(UcTbExecFrame *frame)
{
    frame->tb = NULL;
}

static inline bool tb_exec_frame_resolve(uc_engine *uc,
                                         const TranslationBlock *tb,
                                         uint64_t phys_start[2],
                                         uint32_t phys_size[2])
{
    uint32_t page_offset;
    uint32_t first_size;

    phys_start[0] = 0;
    phys_start[1] = 0;
    phys_size[0] = 0;
    phys_size[1] = 0;

    /* Non-RAM CF_NOCACHE TBs have no physical fragment. */
    if (tb == NULL || tb->page_addr[0] == -1) {
        return false;
    }

    page_offset = tb->pc & uc->target_page_align;
    first_size = MIN((uint32_t)tb->size,
                     uc->target_page_size - page_offset);
    phys_start[0] = tb->page_addr[0] + page_offset;
    phys_size[0] = first_size;
    if (tb->page_addr[1] != -1) {
        phys_start[1] = tb->page_addr[1];
        phys_size[1] = tb->size - first_size;
    }
    return true;
}

static inline bool tb_exec_frame_contains_retaddr(const TranslationBlock *tb,
                                                  uintptr_t retaddr)
{
    uintptr_t start;

    if (tb == NULL || retaddr == 0) {
        return false;
    }
    start = (uintptr_t)tb->tc.ptr;
    return retaddr >= start && retaddr - start < tb->tc.size;
}

static inline bool tb_exec_frame_publish_retaddr(uc_engine *uc,
                                                 uintptr_t retaddr)
{
    UcTbExecFrame *frame = uc->active_tb_exec_frame;
    TranslationBlock *tb;

    if (frame == NULL || !frame->active) {
        return true;
    }
    if (tb_exec_frame_contains_retaddr(frame->tb, retaddr)) {
        return true;
    }

    tb = retaddr != 0 ? tcg_tb_lookup(uc->tcg_ctx, retaddr) : NULL;
    frame->tb = tb;
    return tb != NULL;
}

#endif /* ACCEL_TCG_TB_EXEC_FRAME_H */
