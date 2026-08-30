/* Unicorn Emulator Engine */
/* By Nguyen Anh Quynh <aquynh@gmail.com>, 2015 */
/* Modified for Unicorn Engine by Chen Huitao<chenhuitao@hfmrit.com>, 2020 */

#include "unicorn/unicorn.h"
#if defined(UNICORN_HAS_OSXKERNEL)
#include <libkern/libkern.h>
#else
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#endif

#include <time.h> // nanosleep
#include <string.h>
#ifdef _WIN32
#include <process.h>
#endif

#include "uc_priv.h"

// target specific headers
#include "qemu/target/m68k/unicorn.h"
#include "qemu/target/i386/unicorn.h"
#include "qemu/target/arm/unicorn.h"
#include "qemu/target/mips/unicorn.h"
#include "qemu/target/sparc/unicorn.h"
#include "qemu/target/ppc/unicorn.h"
#include "qemu/target/riscv/unicorn.h"
#include "qemu/target/s390x/unicorn.h"
#include "qemu/target/tricore/unicorn.h"

#include "qemu/include/tcg/tcg-apple-jit.h"
#include "qemu/include/qemu/queue.h"
#include "qemu-common.h"

static void clear_deleted_hooks(uc_engine *uc);
static uc_err uc_snapshot(uc_engine *uc);
static uc_err uc_restore_snapshot_preflight(uc_engine *uc,
                                            const uc_context *context,
                                            FlatView *restore_view);
static UcMapping *mapped_block_at(const uc_engine *uc, uint64_t address);
static void context_memory_clear(uc_context *context, bool reclaim,
                                 bool normalize);

typedef struct UcContextAllocation {
    size_t capacity;
    uc_context *context;
    struct UcContextAllocation *next;
} UcContextAllocation;

static int context_allocations_lock;
static UcContextAllocation *context_allocations;

#ifdef UNICORN_TEST_ALLOC_FAILURE
static bool test_alloc_should_fail(uc_engine *uc,
                                   UcTestAllocFailSite site)
{
    if (uc->test_alloc_fail_site != site) {
        return false;
    }

    uc->test_alloc_fail_site = UC_TEST_ALLOC_FAIL_NONE;
    return true;
}
#else
#define test_alloc_should_fail(uc, site) false
#endif

#define UC_HOOK_MEM_FAST_PATH                                                  \
    (UC_HOOK_MEM_READ | UC_HOOK_MEM_READ_AFTER | UC_HOOK_MEM_WRITE)

#define UC_MTE_TAG_STORAGE_GRANULE 32

#if defined(__APPLE__) && defined(HAVE_PTHREAD_JIT_PROTECT) &&                 \
    (defined(__arm__) || defined(__aarch64__))
static void save_jit_state(uc_engine *uc)
{
    if (!uc->nested) {
        uc->thread_executable_entry = thread_executable();
        uc->current_executable = uc->thread_executable_entry;
    }

    uc->nested += 1;
}

static void restore_jit_state(uc_engine *uc)
{
    assert(uc->nested > 0);
    if (uc->nested == 1) {
        assert_executable(uc->current_executable);
        if (uc->current_executable != uc->thread_executable_entry) {
            if (uc->thread_executable_entry) {
                jit_write_protect(true);
            } else {
                jit_write_protect(false);
            }
        }
    }
    uc->nested -= 1;
}
#else
static void save_jit_state(uc_engine *uc)
{
    (void)uc;
}
static void restore_jit_state(uc_engine *uc)
{
    (void)uc;
}
#endif

static void *hook_insert(struct list *l, struct hook *h)
{
    void *item = list_insert(l, (void *)h);
    if (item) {
        h->refs++;
    }
    return item;
}

static void *hook_append(struct list *l, struct hook *h)
{
    void *item = list_append(l, (void *)h);
    if (item) {
        h->refs++;
    }
    return item;
}

static void hook_invalidate_region(void *key, void *data, void *opaq)
{
    uc_engine *uc = (uc_engine *)opaq;
    HookedRegion *region = (HookedRegion *)key;

    uc->uc_invalidate_tb(uc, region->start, region->length);
}

static void request_tb_flush(uc_engine *uc)
{
    if (uc->tb_exec_depth != 0) {
        uc->tb_flush_pending = true;
        uc->tb_flush_deferred(uc);
    } else {
        uc->tb_flush(uc);
    }
}

static void hook_invalidate_range(uc_engine *uc, uint64_t begin, uint64_t end)
{
    uint64_t span;

    if (end < begin) {
        request_tb_flush(uc);
        return;
    }
    span = end - begin;
    if (span >= SIZE_MAX) {
        request_tb_flush(uc);
        return;
    }
    uc->uc_invalidate_tb(uc, begin, (size_t)span + 1);
}

static void hook_delete(void *data)
{
    struct hook *h = (struct hook *)data;

    h->refs--;

    if (h->refs == 0) {
        g_hash_table_destroy(h->hooked_regions);
        free(h);
    }
}

UNICORN_EXPORT
unsigned int uc_version(unsigned int *major, unsigned int *minor)
{
    if (major != NULL && minor != NULL) {
        *major = UC_API_MAJOR;
        *minor = UC_API_MINOR;
    }

    return (UC_API_MAJOR << 24) + (UC_API_MINOR << 16) + (UC_API_PATCH << 8) +
           UC_API_EXTRA;
}

static uc_err default_reg_read(void *env, int mode, unsigned int regid,
                               void *value, size_t *size)
{
    return UC_ERR_HANDLE;
}

static uc_err default_reg_write(void *env, int mode, unsigned int regid,
                                const void *value, size_t *size, int *setpc)
{
    return UC_ERR_HANDLE;
}

UNICORN_EXPORT
uc_err uc_errno(uc_engine *uc)
{
    return uc->errnum;
}

UNICORN_EXPORT
const char *uc_strerror(uc_err code)
{
    switch (code) {
    default:
        return "Unknown error code";
    case UC_ERR_OK:
        return "OK (UC_ERR_OK)";
    case UC_ERR_NOMEM:
        return "No memory available or memory not present (UC_ERR_NOMEM)";
    case UC_ERR_ARCH:
        return "Invalid/unsupported architecture (UC_ERR_ARCH)";
    case UC_ERR_HANDLE:
        return "Invalid handle (UC_ERR_HANDLE)";
    case UC_ERR_MODE:
        return "Invalid mode (UC_ERR_MODE)";
    case UC_ERR_VERSION:
        return "Different API version between core & binding (UC_ERR_VERSION)";
    case UC_ERR_READ_UNMAPPED:
        return "Invalid memory read (UC_ERR_READ_UNMAPPED)";
    case UC_ERR_WRITE_UNMAPPED:
        return "Invalid memory write (UC_ERR_WRITE_UNMAPPED)";
    case UC_ERR_FETCH_UNMAPPED:
        return "Invalid memory fetch (UC_ERR_FETCH_UNMAPPED)";
    case UC_ERR_HOOK:
        return "Invalid hook type (UC_ERR_HOOK)";
    case UC_ERR_INSN_INVALID:
        return "Invalid instruction (UC_ERR_INSN_INVALID)";
    case UC_ERR_MAP:
        return "Invalid memory mapping (UC_ERR_MAP)";
    case UC_ERR_WRITE_PROT:
        return "Write to write-protected memory (UC_ERR_WRITE_PROT)";
    case UC_ERR_READ_PROT:
        return "Read from non-readable memory (UC_ERR_READ_PROT)";
    case UC_ERR_FETCH_PROT:
        return "Fetch from non-executable memory (UC_ERR_FETCH_PROT)";
    case UC_ERR_ARG:
        return "Invalid argument (UC_ERR_ARG)";
    case UC_ERR_READ_UNALIGNED:
        return "Read from unaligned memory (UC_ERR_READ_UNALIGNED)";
    case UC_ERR_WRITE_UNALIGNED:
        return "Write to unaligned memory (UC_ERR_WRITE_UNALIGNED)";
    case UC_ERR_FETCH_UNALIGNED:
        return "Fetch from unaligned memory (UC_ERR_FETCH_UNALIGNED)";
    case UC_ERR_RESOURCE:
        return "Insufficient resource (UC_ERR_RESOURCE)";
    case UC_ERR_EXCEPTION:
        return "Unhandled CPU exception (UC_ERR_EXCEPTION)";
    case UC_ERR_OVERFLOW:
        return "Provided buffer is too small (UC_ERR_OVERFLOW)";
    case UC_ERR_MMU_READ:
        return "The tlb_fill hook returned false for a read (UC_ERR_MMU_READ)";
    case UC_ERR_MMU_WRITE:
        return "The tlb_fill hook returned false for a write "
               "(UC_ERR_MMU_WRITE)";
    case UC_ERR_MMU_FETCH:
        return "The tlb_fill hook returned false for a fetch "
               "(UC_ERR_MMU_FETCH)";
    }
}

UNICORN_EXPORT
bool uc_arch_supported(uc_arch arch)
{
    switch (arch) {
#ifdef UNICORN_HAS_ARM
    case UC_ARCH_ARM:
        return true;
#endif
#ifdef UNICORN_HAS_ARM64
    case UC_ARCH_ARM64:
        return true;
#endif
#ifdef UNICORN_HAS_M68K
    case UC_ARCH_M68K:
        return true;
#endif
#ifdef UNICORN_HAS_MIPS
    case UC_ARCH_MIPS:
        return true;
#endif
#ifdef UNICORN_HAS_PPC
    case UC_ARCH_PPC:
        return true;
#endif
#ifdef UNICORN_HAS_SPARC
    case UC_ARCH_SPARC:
        return true;
#endif
#ifdef UNICORN_HAS_X86
    case UC_ARCH_X86:
        return true;
#endif
#ifdef UNICORN_HAS_RISCV
    case UC_ARCH_RISCV:
        return true;
#endif
#ifdef UNICORN_HAS_S390X
    case UC_ARCH_S390X:
        return true;
#endif
#ifdef UNICORN_HAS_TRICORE
    case UC_ARCH_TRICORE:
        return true;
#endif
    /* Invalid or disabled arch */
    default:
        return false;
    }
}

#define UC_INIT(uc)                                                            \
    save_jit_state(uc);                                                        \
    if (unlikely(!(uc)->init_done)) {                                          \
        int __init_ret = uc_init_engine(uc);                                   \
        if (unlikely(__init_ret != UC_ERR_OK)) {                               \
            restore_jit_state(uc);                                             \
            return __init_ret;                                                 \
        }                                                                      \
    }

static gint uc_exits_cmp(gconstpointer a, gconstpointer b, gpointer user_data)
{
    uint64_t lhs = *((uint64_t *)a);
    uint64_t rhs = *((uint64_t *)b);

    if (lhs < rhs) {
        return -1;
    } else if (lhs == rhs) {
        return 0;
    } else {
        return 1;
    }
}

static uc_err uc_init_engine(uc_engine *uc)
{
    if (uc->init_done) {
        return UC_ERR_HANDLE;
    }

    uc->hooks_to_del.delete_fn = hook_delete;

    for (int i = 0; i < UC_HOOK_MAX; i++) {
        uc->hook[i].delete_fn = hook_delete;
    }

    uc->ctl_exits = g_tree_new_full(uc_exits_cmp, NULL, g_free, NULL);

    if (machine_initialize(uc)) {
        return UC_ERR_RESOURCE;
    }

    // init tlb function
    if (!uc->cpu->cc->tlb_fill) {
        uc->set_tlb(uc, UC_TLB_CPU);
    }

    // init fpu softfloat
    uc->softfloat_initialize();

    if (uc->reg_reset) {
        uc->reg_reset(uc);
    }

    uc->context_content = UC_CTL_CONTEXT_CPU;

    uc->init_done = true;

    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_open(uc_arch arch, uc_mode mode, uc_engine **result)
{
    struct uc_struct *uc;

    if (arch < UC_ARCH_MAX) {
        uc = calloc(1, sizeof(*uc));
        if (!uc) {
            // memory insufficient
            return UC_ERR_NOMEM;
        }

        /* qemu/exec.c: phys_map_node_reserve() */
        uc->alloc_hint = 16;
        uc->errnum = UC_ERR_OK;
        uc->arch = arch;
        uc->mode = mode;
        uc->reg_read = default_reg_read;
        uc->reg_write = default_reg_write;

        // uc->ram_list = { .blocks = QLIST_HEAD_INITIALIZER(ram_list.blocks) };
        QLIST_INIT(&uc->ram_list.blocks);

        QTAILQ_INIT(&uc->memory_listeners);

        QTAILQ_INIT(&uc->address_spaces);

        switch (arch) {
        default:
            break;
#ifdef UNICORN_HAS_M68K
        case UC_ARCH_M68K:
            if ((mode & ~UC_MODE_M68K_MASK) || !(mode & UC_MODE_BIG_ENDIAN)) {
                free(uc);
                return UC_ERR_MODE;
            }
            uc->init_arch = uc_init_m68k;
            break;
#endif
#ifdef UNICORN_HAS_X86
        case UC_ARCH_X86:
            if ((mode & ~UC_MODE_X86_MASK) || (mode & UC_MODE_BIG_ENDIAN) ||
                !(mode & (UC_MODE_16 | UC_MODE_32 | UC_MODE_64))) {
                free(uc);
                return UC_ERR_MODE;
            }
            uc->init_arch = uc_init_x86_64;
            break;
#endif
#ifdef UNICORN_HAS_ARM
        case UC_ARCH_ARM:
            if ((mode & ~UC_MODE_ARM_MASK)) {
                free(uc);
                return UC_ERR_MODE;
            }
            uc->init_arch = uc_init_arm;

            if (mode & UC_MODE_THUMB) {
                uc->thumb = 1;
            }
            break;
#endif
#ifdef UNICORN_HAS_ARM64
        case UC_ARCH_ARM64:
            if (mode & ~UC_MODE_ARM_MASK) {
                free(uc);
                return UC_ERR_MODE;
            }
            uc->init_arch = uc_init_aarch64;
            break;
#endif

#if defined(UNICORN_HAS_MIPS) || defined(UNICORN_HAS_MIPSEL) ||                \
    defined(UNICORN_HAS_MIPS64) || defined(UNICORN_HAS_MIPS64EL)
        case UC_ARCH_MIPS:
            if ((mode & ~UC_MODE_MIPS_MASK) ||
                !(mode & (UC_MODE_MIPS32 | UC_MODE_MIPS64))) {
                free(uc);
                return UC_ERR_MODE;
            }
            if (((mode & UC_MODE_MICRO) && !(mode & UC_MODE_MIPS32)) ||
                ((mode & UC_MODE_MIPS3) && !(mode & UC_MODE_MIPS64)) ||
                ((mode & UC_MODE_MIPS32R6) && !(mode & UC_MODE_MIPS32))) {
                free(uc);
                return UC_ERR_MODE;
            }
            if (mode & UC_MODE_BIG_ENDIAN) {
#ifdef UNICORN_HAS_MIPS
                if (mode & UC_MODE_MIPS32) {
                    uc->init_arch = uc_init_mips;
                }
#endif
#ifdef UNICORN_HAS_MIPS64
                if (mode & UC_MODE_MIPS64) {
                    uc->init_arch = uc_init_mips64;
                }
#endif
            } else { // little endian
#ifdef UNICORN_HAS_MIPSEL
                if (mode & UC_MODE_MIPS32) {
                    uc->init_arch = uc_init_mipsel;
                }
#endif
#ifdef UNICORN_HAS_MIPS64EL
                if (mode & UC_MODE_MIPS64) {
                    uc->init_arch = uc_init_mips64el;
                }
#endif
            }
            break;
#endif

#ifdef UNICORN_HAS_SPARC
        case UC_ARCH_SPARC:
            if ((mode & ~UC_MODE_SPARC_MASK) || !(mode & UC_MODE_BIG_ENDIAN) ||
                !(mode & (UC_MODE_SPARC32 | UC_MODE_SPARC64))) {
                free(uc);
                return UC_ERR_MODE;
            }
            if (mode & UC_MODE_SPARC64) {
                uc->init_arch = uc_init_sparc64;
            } else {
                uc->init_arch = uc_init_sparc;
            }
            break;
#endif
#ifdef UNICORN_HAS_PPC
        case UC_ARCH_PPC:
            if ((mode & ~UC_MODE_PPC_MASK) || !(mode & UC_MODE_BIG_ENDIAN) ||
                !(mode & (UC_MODE_PPC32 | UC_MODE_PPC64))) {
                free(uc);
                return UC_ERR_MODE;
            }
            if (mode & UC_MODE_PPC64) {
                uc->init_arch = uc_init_ppc64;
            } else {
                uc->init_arch = uc_init_ppc;
            }
            break;
#endif
#ifdef UNICORN_HAS_RISCV
        case UC_ARCH_RISCV:
            if ((mode & ~UC_MODE_RISCV_MASK) ||
                !(mode & (UC_MODE_RISCV32 | UC_MODE_RISCV64))) {
                free(uc);
                return UC_ERR_MODE;
            }
            if (mode & UC_MODE_RISCV32) {
                uc->init_arch = uc_init_riscv32;
            } else if (mode & UC_MODE_RISCV64) {
                uc->init_arch = uc_init_riscv64;
            } else {
                free(uc);
                return UC_ERR_MODE;
            }
            break;
#endif
#ifdef UNICORN_HAS_S390X
        case UC_ARCH_S390X:
            if ((mode & ~UC_MODE_S390X_MASK) || !(mode & UC_MODE_BIG_ENDIAN)) {
                free(uc);
                return UC_ERR_MODE;
            }
            uc->init_arch = uc_init_s390x;
            break;
#endif
#ifdef UNICORN_HAS_TRICORE
        case UC_ARCH_TRICORE:
            if ((mode & ~UC_MODE_TRICORE_MASK)) {
                free(uc);
                return UC_ERR_MODE;
            }
            uc->init_arch = uc_init_tricore;
            break;
#endif
        }

        if (uc->init_arch == NULL) {
            free(uc);
            return UC_ERR_ARCH;
        }

        uc->init_done = false;
        uc->cpu_model = INT_MAX; // INT_MAX means the default cpu model.

        *result = uc;

        return UC_ERR_OK;
    } else {
        return UC_ERR_ARCH;
    }
}

UNICORN_EXPORT
uc_err uc_close(uc_engine *uc)
{
    int i;
    MemoryRegion *mr;
    UcMapping *mapping, *next_mapping;

    if (!uc->init_done) {
        free(uc);
        return UC_ERR_OK;
    }

    // Flush all translation buffers or we leak memory allocated by MMU
    uc->tb_flush(uc);

    for (mapping = uc->mapping_records; mapping; mapping = mapping->next) {
        if (mapping->active) {
            uc->memory_moveout(uc, mapping, true);
            mapping->active = false;
        }
    }

    while (uc->memory_contexts) {
        context_memory_clear(uc->memory_contexts, false, false);
    }

    // Cleanup internally.
    if (uc->release) {
        uc->release(uc->tcg_ctx);
    }
    g_free(uc->tcg_ctx);

    // Cleanup CPU.
    g_free(uc->cpu->cpu_ases);
    g_free(uc->cpu->thread);

    /* cpu */
    qemu_vfree(uc->cpu);

    /* flatviews */
    g_hash_table_destroy(uc->flat_views);

    // During flatviews destruction, we may still access memory regions.
    // So we free them afterwards.
    /* memory */
    mr = &uc->io_mem_unassigned;
    mr->destructor(mr);
    mr = uc->system_io;
    mr->destructor(mr);
    mr = uc->system_memory;
    mr->destructor(mr);
    g_free(uc->system_memory);
    g_free(uc->system_io);
    for (mapping = uc->mapping_records; mapping; mapping = next_mapping) {
        next_mapping = mapping->next;
        uc->memory_mapping_free(mapping);
        g_free(mapping);
    }

    // Thread relateds.
    if (uc->qemu_thread_data) {
        g_free(uc->qemu_thread_data);
    }

    /* free */
    g_free(uc->init_target_page);

    // Other auxilaries.
    g_free(uc->l1_map);

    if (uc->bounce.buffer) {
        qemu_vfree(uc->bounce.buffer);
    }

    // free hooks and hook lists
    clear_deleted_hooks(uc);

    for (i = 0; i < UC_HOOK_MAX; i++) {
        list_clear(&uc->hook[i]);
    }

    g_free(uc->mapped_blocks);

    g_tree_destroy(uc->ctl_exits);

    // finally, free uc itself.
    memset(uc, 0, sizeof(*uc));
    free(uc);

    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_reg_read_batch(uc_engine *uc, int const *regs, void **vals, int count)
{
    UC_INIT(uc);
    reg_read_t reg_read = uc->reg_read;
    void *env = uc->cpu->env_ptr;
    int mode = uc->mode;
    int i;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        void *value = vals[i];
        size_t size = (size_t)-1;
        uc_err err = reg_read(env, mode, regid, value, &size);
        if (err) {
            restore_jit_state(uc);
            return err;
        }
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

static void uc_request_pc_change(uc_engine *uc)
{
    uc->quit_request = true;
    uc->skip_sync_pc_on_exit = true;
    break_translation_loop(uc);
}

UNICORN_EXPORT
uc_err uc_reg_write_batch(uc_engine *uc, int const *regs, void *const *vals,
                          int count)
{
    UC_INIT(uc);
    reg_write_t reg_write = uc->reg_write;
    void *env = uc->cpu->env_ptr;
    int mode = uc->mode;
    int setpc = 0;
    int i;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        const void *value = vals[i];
        size_t size = (size_t)-1;
        uc_err err = reg_write(env, mode, regid, value, &size, &setpc);
        if (err) {
            restore_jit_state(uc);
            return err;
        }
    }
    if (setpc) {
        uc_request_pc_change(uc);
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_reg_read_batch2(uc_engine *uc, int const *regs, void *const *vals,
                          size_t *sizes, int count)
{
    UC_INIT(uc);
    reg_read_t reg_read = uc->reg_read;
    void *env = uc->cpu->env_ptr;
    int mode = uc->mode;
    int i;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        void *value = vals[i];
        uc_err err = reg_read(env, mode, regid, value, sizes + i);
        if (err) {
            restore_jit_state(uc);
            return err;
        }
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_reg_write_batch2(uc_engine *uc, int const *regs,
                           const void *const *vals, size_t *sizes, int count)
{
    UC_INIT(uc);
    reg_write_t reg_write = uc->reg_write;
    void *env = uc->cpu->env_ptr;
    int mode = uc->mode;
    int setpc = 0;
    int i;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        const void *value = vals[i];
        uc_err err = reg_write(env, mode, regid, value, sizes + i, &setpc);
        if (err) {
            restore_jit_state(uc);
            return err;
        }
    }
    if (setpc) {
        uc_request_pc_change(uc);
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_reg_read(uc_engine *uc, int regid, void *value)
{
    UC_INIT(uc);
    size_t size = (size_t)-1;
    uc_err err = uc->reg_read(uc->cpu->env_ptr, uc->mode, regid, value, &size);
    restore_jit_state(uc);
    return err;
}

UNICORN_EXPORT
uc_err uc_reg_write(uc_engine *uc, int regid, const void *value)
{
    UC_INIT(uc);
    int setpc = 0;
    size_t size = (size_t)-1;
    uc_err err =
        uc->reg_write(uc->cpu->env_ptr, uc->mode, regid, value, &size, &setpc);
    if (err) {
        restore_jit_state(uc);
        return err;
    }
    if (setpc) {
        uc_request_pc_change(uc);
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_reg_read2(uc_engine *uc, int regid, void *value, size_t *size)
{
    UC_INIT(uc);
    uc_err err = uc->reg_read(uc->cpu->env_ptr, uc->mode, regid, value, size);
    restore_jit_state(uc);
    return err;
}

UNICORN_EXPORT
uc_err uc_reg_write2(uc_engine *uc, int regid, const void *value, size_t *size)
{
    UC_INIT(uc);
    int setpc = 0;
    uc_err err =
        uc->reg_write(uc->cpu->env_ptr, uc->mode, regid, value, size, &setpc);
    if (err) {
        restore_jit_state(uc);
        return err;
    }
    if (setpc) {
        uc_request_pc_change(uc);
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

static uint64_t memory_region_len(uc_engine *uc, MemoryRegion *mr,
                                  uint64_t address, uint64_t count)
{
    hwaddr end = mr->end;
    while (mr->container != uc->system_memory) {
        mr = mr->container;
        end += mr->addr;
    }
    return (uint64_t)MIN(count, end - address);
}

// check if a memory area is mapped
// this is complicated because an area can overlap adjacent blocks
static bool check_mem_area(uc_engine *uc, uint64_t address, size_t size,
                           MemoryRegion **first_mr)
{
    size_t count = 0, len;

    if (first_mr != NULL) {
        *first_mr = NULL;
    }

    // A wrap-around range can never be a single mapped extent. Reject it
    // here so the loop below can't walk from the top of the address space
    // back to 0 and convince callers that a wrapping range is "valid".
    // uc_mem_map() rejects the same condition in mem_map_check().
    if (size != 0 && (address + size - 1) < address) {
        return false;
    }

    while (count < size) {
        MemoryRegion *mr = uc->memory_mapping(uc, address);
        if (mr) {
            if (count == 0 && first_mr != NULL) {
                *first_mr = mr;
            }
            len = memory_region_len(uc, mr, address, size - count);
            count += len;
            address += len;
        } else { // this address is not mapped in yet
            break;
        }
    }

    return (count == size);
}

uc_err uc_vmem_translate(uc_engine *uc, uint64_t address, uc_prot prot,
                         uint64_t *paddress)
{
    UC_INIT(uc);

    if (!(UC_PROT_READ == prot || UC_PROT_WRITE == prot ||
          UC_PROT_EXEC == prot)) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    // The sparc mmu doesn't support probe mode
    if (uc->arch == UC_ARCH_SPARC &&
        uc->cpu->cc->tlb_fill == uc->cpu->cc->tlb_fill_cpu) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    if (!uc->virtual_to_physical(uc, address, prot, paddress)) {
        restore_jit_state(uc);
        switch (prot) {
        case UC_PROT_READ:
            return UC_ERR_READ_PROT;
        case UC_PROT_WRITE:
            return UC_ERR_WRITE_PROT;
        case UC_PROT_EXEC:
            return UC_ERR_FETCH_PROT;
        default:
            return UC_ERR_ARG;
        }
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_vmem_read(uc_engine *uc, uint64_t address, uc_prot prot, void *_bytes,
                    size_t size)
{
    size_t count = 0, len;
    uint8_t *bytes = _bytes;
    uint64_t align;
    uint64_t pagesize;

    UC_INIT(uc);

    // qemu cpu_physical_memory_rw() size is an int
    if (size > INT_MAX) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    // The sparc mmu doesn't support probe mode
    if (uc->arch == UC_ARCH_SPARC &&
        uc->cpu->cc->tlb_fill == uc->cpu->cc->tlb_fill_cpu) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    if (!(UC_PROT_READ == prot || UC_PROT_WRITE == prot ||
          UC_PROT_EXEC == prot)) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    while (count < size) {
        align = uc->target_page_align;
        pagesize = uc->target_page_size;
        len = MIN(size - count, (address & ~align) + pagesize - address);
        if (!uc->read_mem_virtual(uc, address, prot, bytes, len)) {
            restore_jit_state(uc);
            return UC_ERR_READ_PROT;
        }
        bytes += len;
        address += len;
        count += len;
    }
    assert(count == size);
    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_vmem_write(uc_engine *uc, uint64_t address, uc_prot prot,
                     const void *_bytes, size_t size)
{
    size_t count = 0, len;
    const uint8_t *bytes = _bytes;
    uint64_t align;
    uint64_t pagesize;
    uint64_t paddr = 0;

    UC_INIT(uc);

    // qemu cpu_physical_memory_rw() size is an int
    if (size > INT_MAX) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    // The sparc mmu doesn't support probe mode
    if (uc->arch == UC_ARCH_SPARC &&
        uc->cpu->cc->tlb_fill == uc->cpu->cc->tlb_fill_cpu) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    if (!(UC_PROT_READ == prot || UC_PROT_WRITE == prot ||
          UC_PROT_EXEC == prot)) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    while (count < size) {
        align = uc->target_page_align;
        pagesize = uc->target_page_size;
        len = MIN(size - count, (address & ~align) + pagesize - address);
        if (uc_vmem_translate(uc, address, prot, &paddr) != UC_ERR_OK) {
            restore_jit_state(uc);
            return UC_ERR_WRITE_PROT;
        }
        if (uc_mem_write(uc, paddr, bytes, len) != UC_ERR_OK) {
            restore_jit_state(uc);
            return UC_ERR_WRITE_PROT;
        }
        bytes += len;
        address += len;
        count += len;
    }
    assert(count == size);
    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_mem_read(uc_engine *uc, uint64_t address, void *_bytes, uint64_t size)
{
    uint64_t count = 0, len;
    uint8_t *bytes = _bytes;
    MemoryRegion *first_mr;

    UC_INIT(uc);

    if (!check_mem_area(uc, address, size, &first_mr)) {
        restore_jit_state(uc);
        return UC_ERR_READ_UNMAPPED;
    }

    // memory area can overlap adjacent memory blocks
    while (count < size) {
        MemoryRegion *mr =
            count == 0 ? first_mr : uc->memory_mapping(uc, address);
        if (mr) {
            len = memory_region_len(uc, mr, address, size - count);
            if (uc->read_mem(&uc->address_space_memory, address, bytes, len) ==
                false) {
                break;
            }
            count += len;
            address += len;
            bytes += len;
        } else { // this address is not mapped in yet
            break;
        }
    }

    if (count == size) {
        restore_jit_state(uc);
        return UC_ERR_OK;
    } else {
        restore_jit_state(uc);
        return UC_ERR_READ_UNMAPPED;
    }
}

static bool ranges_overlap(uint64_t first_start, uint64_t first_size,
                           uint64_t second_start, uint64_t second_size)
{
    if (first_size == 0 || second_size == 0) {
        return false;
    }
    if (first_start <= second_start) {
        return second_start - first_start < first_size;
    }
    return first_start - second_start < second_size;
}

static uint64_t mem_write_active_tb_mask(uc_engine *uc, MemoryRegion *mr,
                                         uint64_t address, uint64_t size)
{
    uint64_t active_mask = 0;
    unsigned int i;

    if (size == 0 || uc->tb_exec_depth == 0) {
        return 0;
    }

    if (mr->ram && mr->ram_block != NULL) {
        MemoryRegion *container = mr;
        uint64_t region_address = mr->addr;
        ram_addr_t ram_address;

        while (container->container != uc->system_memory) {
            container = container->container;
            region_address += container->addr;
        }
        ram_address = mr->ram_block->offset + (address - region_address);
        for (i = 0; i < UC_MAX_NESTED_LEVEL; i++) {
            UcTbExecFrame *frame = &uc->tb_exec_frames[i];
            uint64_t phys_start[2];
            uint32_t phys_size[2];
            unsigned int page;

            if (!frame->active) {
                continue;
            }
            if (frame->tb == NULL) {
                active_mask |= 1ULL << i;
                continue;
            }
            if (!uc->tb_exec_frame_resolve(uc, frame->tb, phys_start,
                                           phys_size)) {
                continue;
            }
            for (page = 0; page < 2; page++) {
                if (ranges_overlap(ram_address, size, phys_start[page],
                                   phys_size[page])) {
                    active_mask |= 1ULL << i;
                    break;
                }
            }
        }
    }

    return active_mask;
}

static uint64_t all_active_tb_mask(uc_engine *uc)
{
    uint64_t active_mask = 0;
    unsigned int i;

    for (i = 0; i < UC_MAX_NESTED_LEVEL; i++) {
        if (uc->tb_exec_frames[i].active) {
            active_mask |= 1ULL << i;
        }
    }
    return active_mask;
}

static void mark_active_tbs_for_exit(uc_engine *uc, uint64_t active_tb_mask)
{
    unsigned int i;

    for (i = 0; i < UC_MAX_NESTED_LEVEL; i++) {
        if ((active_tb_mask & (1ULL << i)) != 0) {
            uc->tb_exec_frames[i].exit_requested = true;
        }
    }
}

static void mem_write_finalize_active_tbs(uc_engine *uc,
                                          uint64_t active_tb_mask)
{
    unsigned int current_level;

    if (active_tb_mask == 0) {
        return;
    }
    mark_active_tbs_for_exit(uc, active_tb_mask);
    request_tb_flush(uc);

    if (uc->nested_level == 0) {
        return;
    }
    current_level = uc->nested_level - 1;
    if ((active_tb_mask & (1ULL << current_level)) == 0) {
        return;
    }
    uc->quit_request = true;
    break_translation_loop(uc);
}

UNICORN_EXPORT
uc_err uc_mem_write(uc_engine *uc, uint64_t address, const void *_bytes,
                    uint64_t size)
{
    uint64_t count = 0, len;
    const uint8_t *bytes = _bytes;
    MemoryRegion *first_mr;
    uint64_t active_tb_mask = 0;

    UC_INIT(uc);

    if (!check_mem_area(uc, address, size, &first_mr)) {
        restore_jit_state(uc);
        return UC_ERR_WRITE_UNMAPPED;
    }

    // memory area can overlap adjacent memory blocks
    while (count < size) {
        MemoryRegion *mr =
            count == 0 ? first_mr : uc->memory_mapping(uc, address);
        if (mr) {
            UcMapping *mapping = mapped_block_at(uc, address);
            MemoryRegion *original_mr = mr;
            uint64_t chunk_active_tb_mask = 0;
            uint64_t cow_active_tb_mask = 0;
            uint32_t operms = mr->perms;
            uint64_t align = uc->target_page_align;
            if (!(operms & UC_PROT_WRITE)) { // write protected
                // but this is not the program accessing memory, so temporarily
                // mark writable
                uc->readonly_mem(mr, false);
            }

            len = memory_region_len(uc, mr, address, size - count);
            chunk_active_tb_mask =
                mem_write_active_tb_mask(uc, mr, address, len);
            if (mr->ram && uc->memory_context_count != 0 &&
                uc->snapshot_level > mr->priority) {
                uint64_t cow_address = address & ~align;
                uint64_t cow_size = (len + (address & align) + align) & ~align;

                cow_active_tb_mask =
                    mem_write_active_tb_mask(uc, mr, cow_address, cow_size);
                assert(mapping != NULL);
                mr = uc->memory_cow(uc, mapping, mr, cow_address, cow_size);
                if (!mr) {
                    if (!(operms & UC_PROT_WRITE)) {
                        uc->readonly_mem(original_mr, true);
                    }
                    mem_write_finalize_active_tbs(uc, active_tb_mask);
                    restore_jit_state(uc);
                    return UC_ERR_NOMEM;
                }
                active_tb_mask |= cow_active_tb_mask;
            }
            if (uc->write_mem(&uc->address_space_memory, address, bytes, len) ==
                false) {
                break;
            }
            active_tb_mask |= chunk_active_tb_mask;

            if (!(operms & UC_PROT_WRITE)) { // write protected
                // now write protect it again
                uc->readonly_mem(mr, true);
            }

            count += len;
            address += len;
            bytes += len;
        } else { // this address is not mapped in yet
            break;
        }
    }

    mem_write_finalize_active_tbs(uc, active_tb_mask);

    if (count == size) {
        restore_jit_state(uc);
        return UC_ERR_OK;
    } else {
        restore_jit_state(uc);
        return UC_ERR_WRITE_UNMAPPED;
    }
}

#define TIMEOUT_STEP 2 // microseconds
typedef enum UcEmuFrameState {
    UC_EMU_FRAME_INACTIVE,
    UC_EMU_FRAME_PREPARED,
    UC_EMU_FRAME_ACTIVE,
    UC_EMU_FRAME_DONE,
    UC_EMU_FRAME_TIMED_OUT,
} UcEmuFrameState;

static int emu_atomic_read(int *value)
{
#ifdef _MSC_VER
    return InterlockedCompareExchange((volatile LONG *)value, 0, 0);
#else
    return qatomic_cmpxchg(value, 0, 0);
#endif
}

static void emu_atomic_set(int *value, int new_value)
{
#ifdef _MSC_VER
    InterlockedExchange((volatile LONG *)value, new_value);
#else
    qatomic_xchg(value, new_value);
#endif
}

static int emu_atomic_cmpxchg(int *value, int old_value, int new_value)
{
#ifdef _MSC_VER
    return InterlockedCompareExchange((volatile LONG *)value, new_value,
                                      old_value);
#else
    return qatomic_cmpxchg(value, old_value, new_value);
#endif
}

static bool emu_ancestor_timed_out(uc_engine *uc, int level)
{
    int i;

    for (i = 0; i < level; i++) {
        if (emu_atomic_read(&uc->emu_frames[i].state) ==
            UC_EMU_FRAME_TIMED_OUT) {
            return true;
        }
    }
    return false;
}

static void *_timeout_fn(void *arg)
{
    UcEmuFrame *frame = (UcEmuFrame *)arg;
    struct uc_struct *uc = frame->uc;

    while (emu_atomic_read(&frame->state) == UC_EMU_FRAME_PREPARED) {
        usleep(TIMEOUT_STEP);
    }
    while (emu_atomic_read(&frame->state) == UC_EMU_FRAME_ACTIVE) {
        int64_t current_time = get_clock();

        if ((uint64_t)(current_time - frame->start_time) >= frame->timeout &&
            emu_atomic_cmpxchg(&frame->state, UC_EMU_FRAME_ACTIVE,
                               UC_EMU_FRAME_TIMED_OUT) == UC_EMU_FRAME_ACTIVE) {
            emu_atomic_set(&uc->timed_out, true);
            uc_set_stop_request(uc, true);
            if (uc->cpu) {
                cpu_exit(uc->cpu);
            }
            break;
        }
        if (emu_atomic_read(&frame->state) == UC_EMU_FRAME_ACTIVE) {
            usleep(TIMEOUT_STEP);
        }
    }

    return NULL;
}

#ifdef _WIN32
static unsigned __stdcall timeout_fn_win32(void *arg)
{
    _timeout_fn(arg);
    return 0;
}
#endif

static uc_err enable_emu_timer(UcEmuFrame *frame)
{
#ifdef _WIN32
    frame->timer_handle =
        _beginthreadex(NULL, 0, timeout_fn_win32, frame, 0, NULL);
    if (frame->timer_handle == 0) {
        return UC_ERR_RESOURCE;
    }
#else
    if (qemu_thread_create(frame->uc, &frame->timer, "timeout", _timeout_fn,
                           frame, QEMU_THREAD_JOINABLE) != 0) {
        return UC_ERR_RESOURCE;
    }
#endif
    frame->timer_started = true;
    return UC_ERR_OK;
}

static void join_emu_timer(UcEmuFrame *frame)
{
    if (!frame->timer_started) {
        return;
    }
#ifdef _WIN32
    WaitForSingleObject((HANDLE)frame->timer_handle, INFINITE);
    CloseHandle((HANDLE)frame->timer_handle);
    frame->timer_handle = 0;
#else
    qemu_thread_join(&frame->timer);
#endif
    frame->timer_started = false;
}

static void hook_count_cb(struct uc_struct *uc, uint64_t address, uint32_t size,
                          void *user_data)
{
    uc->emu_counter++;
    if (uc->emu_counter > uc->emu_count) {
        uc_emu_stop(uc);
    }
}

static void clear_deleted_hooks(uc_engine *uc)
{
    struct list_item *cur;
    struct hook *hook;
    int i;

    for (cur = uc->hooks_to_del.head;
         cur != NULL && (hook = (struct hook *)cur->data); cur = cur->next) {
        assert(hook->to_delete);
        for (i = 0; i < UC_HOOK_MAX; i++) {
            if (list_remove(&uc->hook[i], (void *)hook)) {
                break;
            }
        }
    }

    list_clear(&uc->hooks_to_del);
}

UNICORN_EXPORT
uc_err uc_emu_start(uc_engine *uc, uint64_t begin, uint64_t until,
                    uint64_t timeout, size_t count)
{
    uc_err err = UC_ERR_OK;
    bool nested_start;
    size_t saved_emu_count = 0;
    size_t saved_emu_counter = 0;
    IcountDecr saved_icount_decr = {0};
    uint32_t saved_cflags_next_tb = 0;
    uc_tb saved_last_tb = {0};
    bool saved_last_tb_valid = false;
    bool saved_stop_request = false;
    bool saved_quit_request = false;
    bool parent_tb_exit_requested = false;
    bool ancestor_timeout_expired = false;
    bool frame_timed_out;
    uint64_t saved_timeout = uc->timeout;
    UcEmuFrame *frame;
    int frame_state;
    int frame_level;

    // Reject before changing lifecycle state owned by the active frame.
    if (uc->nested_level >= UC_MAX_NESTED_LEVEL) {
        return UC_ERR_RESOURCE;
    }
    nested_start = uc->nested_level != 0;
    if (nested_start) {
        saved_emu_count = uc->emu_count;
        saved_emu_counter = uc->emu_counter;
        saved_icount_decr = *uc->cpu->icount_decr_ptr;
        saved_cflags_next_tb = uc->cpu->cflags_next_tb;
        saved_last_tb = uc->last_tb;
        saved_last_tb_valid = uc->last_tb_valid;
        saved_stop_request = uc_stop_requested(uc);
        saved_quit_request = uc->quit_request;
    }

    // Avoid nested uc_emu_start saves wrong jit states.
    if (uc->nested_level == 0) {
        UC_INIT(uc);
    }

    frame_level = uc->nested_level;
    frame = &uc->emu_frames[frame_level];
    frame->uc = uc;
    frame->timeout = timeout * 1000; // microseconds -> nanoseconds
    frame->descendant_timed_out = false;
    frame->timer_started = false;
    emu_atomic_set(&frame->state, UC_EMU_FRAME_PREPARED);
    if (timeout) {
        err = enable_emu_timer(frame);
        if (err != UC_ERR_OK) {
            emu_atomic_set(&frame->state, UC_EMU_FRAME_DONE);
            if (!nested_start) {
                restore_jit_state(uc);
            }
            return err;
        }
    }

    if (nested_start) {
        uc->last_tb_valid = false;
    }

    // reset the counter
    uc->emu_counter = 0;
    uc->invalid_error = UC_ERR_OK;
    uc->emulation_done = false;
    uc->size_recur_mem = 0;
    emu_atomic_set(&uc->timed_out, false);
    uc->first_tb = true;

    // Advance the nested levels. We must decrease the level count by one when
    // we return from uc_emu_start.
    uc->nested_level++;

    uint32_t begin_pc32 = READ_DWORD(begin);
    switch (uc->arch) {
    default:
        break;
#ifdef UNICORN_HAS_M68K
    case UC_ARCH_M68K:
        uc_reg_write(uc, UC_M68K_REG_PC, &begin_pc32);
        break;
#endif
#ifdef UNICORN_HAS_X86
    case UC_ARCH_X86:
        switch (uc->mode) {
        default:
            break;
        case UC_MODE_16: {
            uint16_t ip;
            uint16_t cs;

            uc_reg_read(uc, UC_X86_REG_CS, &cs);
            // compensate for later adding up IP & CS
            ip = begin - cs * 16;
            uc_reg_write(uc, UC_X86_REG_IP, &ip);
            break;
        }
        case UC_MODE_32:
            uc_reg_write(uc, UC_X86_REG_EIP, &begin_pc32);
            break;
        case UC_MODE_64:
            uc_reg_write(uc, UC_X86_REG_RIP, &begin);
            break;
        }
        break;
#endif
#ifdef UNICORN_HAS_ARM
    case UC_ARCH_ARM:
        uc_reg_write(uc, UC_ARM_REG_R15, &begin_pc32);
        break;
#endif
#ifdef UNICORN_HAS_ARM64
    case UC_ARCH_ARM64:
        uc_reg_write(uc, UC_ARM64_REG_PC, &begin);
        break;
#endif
#ifdef UNICORN_HAS_MIPS
    case UC_ARCH_MIPS:
        if (uc->mode & UC_MODE_MIPS64) {
            uc_reg_write(uc, UC_MIPS_REG_PC, &begin);
        } else {
            uc_reg_write(uc, UC_MIPS_REG_PC, &begin_pc32);
        }
        break;
#endif
#ifdef UNICORN_HAS_SPARC
    case UC_ARCH_SPARC:
        // TODO: Sparc/Sparc64
        uc_reg_write(uc, UC_SPARC_REG_PC, &begin);
        break;
#endif
#ifdef UNICORN_HAS_PPC
    case UC_ARCH_PPC:
        if (uc->mode & UC_MODE_PPC64) {
            uc_reg_write(uc, UC_PPC_REG_PC, &begin);
        } else {
            uc_reg_write(uc, UC_PPC_REG_PC, &begin_pc32);
        }
        break;
#endif
#ifdef UNICORN_HAS_RISCV
    case UC_ARCH_RISCV:
        if (uc->mode & UC_MODE_RISCV64) {
            uc_reg_write(uc, UC_RISCV_REG_PC, &begin);
        } else {
            uc_reg_write(uc, UC_RISCV_REG_PC, &begin_pc32);
        }
        break;
#endif
#ifdef UNICORN_HAS_S390X
    case UC_ARCH_S390X:
        uc_reg_write(uc, UC_S390X_REG_PC, &begin);
        break;
#endif
#ifdef UNICORN_HAS_TRICORE
    case UC_ARCH_TRICORE:
        uc_reg_write(uc, UC_TRICORE_REG_PC, &begin_pc32);
        break;
#endif
    }
    uc->skip_sync_pc_on_exit = false;
    revert_uc_emu_stop(uc);

    uc->emu_count = count;
    uc->cpu->icount_decr_ptr->u32 = 0;
    uc->cpu->icount_extra = 0;
    uc->cpu->icount_budget = 0;
    if (uc_uses_tcg_count(uc)) {
        size_t initial_count = MIN((size_t)UINT16_MAX, count);

        uc->cpu->icount_decr_ptr->u16.low = initial_count;
        uc->emu_counter = count - initial_count;
    }
    if (count == 0 && uc->count_hook != 0) {
        uc_hook_del(uc, uc->count_hook);
        uc->count_hook = 0;
        uc->tb_flush(uc);
    } else if (count != 0 && !uc_uses_tcg_count(uc) && uc->count_hook == 0) {
        uc->hook_insert = 1;
        err = uc_hook_add(uc, &uc->count_hook, UC_HOOK_CODE, hook_count_cb,
                          NULL, 1, 0);
        uc->hook_insert = 0;
        if (err != UC_ERR_OK) {
            frame_state = emu_atomic_cmpxchg(
                &frame->state, UC_EMU_FRAME_PREPARED, UC_EMU_FRAME_DONE);
            goto frame_done;
        }
    }

    // If UC_CTL_UC_USE_EXITS is set, then the @until param won't have any
    // effect. This is designed for the backward compatibility.
    if (!uc->use_exits) {
        uc->exits[uc->nested_level - 1] = until;
    }

    frame->start_time = get_clock();
    uc->timeout = frame->timeout;
    emu_atomic_set(&frame->state, UC_EMU_FRAME_ACTIVE);
    if (emu_ancestor_timed_out(uc, frame_level)) {
        emu_atomic_set(&uc->timed_out, true);
        uc_set_stop_request(uc, true);
        break_translation_loop(uc);
    }

    uc->vm_start(uc);

    frame_state = emu_atomic_cmpxchg(&frame->state, UC_EMU_FRAME_ACTIVE,
                                     UC_EMU_FRAME_DONE);
frame_done:
    join_emu_timer(frame);

    ancestor_timeout_expired = emu_ancestor_timed_out(uc, frame_level);
    frame_timed_out = frame->descendant_timed_out || ancestor_timeout_expired ||
                      frame_state == UC_EMU_FRAME_TIMED_OUT;
    emu_atomic_set(&uc->timed_out, frame_timed_out);

    uc->nested_level--;
    if (nested_start) {
        UcEmuFrame *parent = &uc->emu_frames[frame_level - 1];

        /* A nested write may have invalidated the suspended parent TB. */
        parent_tb_exit_requested =
            uc->tb_exec_frames[frame_level - 1].exit_requested;
        parent->descendant_timed_out |= frame_timed_out;
        revert_uc_emu_stop(uc);
        ancestor_timeout_expired |= emu_ancestor_timed_out(uc, frame_level);
        if (ancestor_timeout_expired) {
            frame_timed_out = true;
            parent->descendant_timed_out = true;
            emu_atomic_set(&uc->timed_out, true);
        }
        uc->quit_request = saved_quit_request || parent_tb_exit_requested;
        uc->timeout = saved_timeout;
        uc->emu_count = saved_emu_count;
        uc->emu_counter = saved_emu_counter;
        *uc->cpu->icount_decr_ptr = saved_icount_decr;
        uc->cpu->cflags_next_tb = saved_cflags_next_tb;
        uc->last_tb = saved_last_tb;
        uc->last_tb_valid = saved_last_tb_valid;
        if (saved_stop_request || ancestor_timeout_expired) {
            uc_set_stop_request(uc, true);
        }
        if (saved_stop_request || saved_quit_request ||
            ancestor_timeout_expired || parent_tb_exit_requested) {
            break_translation_loop(uc);
        }
    } else {
        uc->emu_count = 0;
        uc->cpu->icount_decr_ptr->u32 = 0;
        uc->cpu->icount_extra = 0;
        uc->cpu->icount_budget = 0;
    }
    if (uc->nested_level == 0 && uc->tb_flush_pending) {
        uc->tb_flush_pending = false;
        uc->tb_flush(uc);
    }

    // emulation is done if and only if we exit the outer uc_emu_start
    // or we may lost uc_emu_stop
    if (uc->nested_level == 0) {
        uc->emulation_done = true;

        // remove hooks to delete
        // make sure we delete all hooks at the first level.
        clear_deleted_hooks(uc);

        restore_jit_state(uc);
    }

    // We may be in a nested uc_emu_start and thus clear invalid_error
    // once we are done.
    if (err == UC_ERR_OK) {
        err = uc->invalid_error;
    }
    uc->invalid_error = 0;
    return err;
}

UNICORN_EXPORT
uc_err uc_emu_stop(uc_engine *uc)
{
    UC_INIT(uc);
    uc_set_stop_request(uc, true);
    uc_err err = break_translation_loop(uc);
    restore_jit_state(uc);
    return err;
}

// return target index where a memory region at the address exists, or could be
// inserted
//
// address either is inside the mapping at the returned index, or is in free
// space before the next mapping.
//
// if there is overlap, between regions, ending address will be higher than the
// starting address of the mapping at returned index
static int bsearch_mapped_blocks(const uc_engine *uc, uint64_t address)
{
    int left, right, mid;
    UcMapping *mapping;

    left = 0;
    right = uc->mapped_block_count;

    while (left < right) {
        mid = left + (right - left) / 2;

        mapping = uc->mapped_blocks[mid];

        if (address < mapping->begin) {
            right = mid;
        } else if (address - mapping->begin >= mapping->size) {
            left = mid + 1;
        } else {
            return mid;
        }
    }

    return left;
}

static UcMapping *mapped_block_at(const uc_engine *uc, uint64_t address)
{
    int index = bsearch_mapped_blocks(uc, address);

    if (index >= 0 && (uint32_t)index < uc->mapped_block_count) {
        UcMapping *mapping = uc->mapped_blocks[index];

        if (mapping->begin <= address &&
            address - mapping->begin < mapping->size) {
            return mapping;
        }
    }
    return NULL;
}

// find if a memory range overlaps with existing mapped regions
static bool memory_overlap(struct uc_struct *uc, uint64_t begin, uint64_t size)
{
    unsigned int i;

    i = bsearch_mapped_blocks(uc, begin);

    // is this the highest region with no possible overlap?
    if (i >= uc->mapped_block_count) {
        return false;
    }

    return ranges_overlap(begin, size, uc->mapped_blocks[i]->begin,
                          uc->mapped_blocks[i]->size);
}

static bool mem_map_reserve(uc_engine *uc, uint32_t count)
{
    UcMapping **mappings;
    uint32_t capacity;

    if (count <= uc->mapped_block_capacity) {
        return true;
    }
    if (count > UINT32_MAX - (MEM_BLOCK_INCR - 1)) {
        return false;
    }

    capacity = (count + MEM_BLOCK_INCR - 1) & ~(MEM_BLOCK_INCR - 1);
    if (test_alloc_should_fail(uc, UC_TEST_ALLOC_FAIL_MAPPED_BLOCKS)) {
        return false;
    }
    mappings = g_try_new(UcMapping *, capacity);
    if (!mappings) {
        return false;
    }
    if (uc->mapped_block_count != 0) {
        memcpy(mappings, uc->mapped_blocks,
               sizeof(*mappings) * uc->mapped_block_count);
    }
    g_free(uc->mapped_blocks);
    uc->mapped_blocks = mappings;
    uc->mapped_block_capacity = capacity;
    return true;
}

static UcMapping *mem_map_prepare(uc_engine *uc, uint64_t begin, uint64_t size)
{
    UcMapping *mapping;

    if (!mem_map_reserve(uc, uc->mapped_block_count + 1)) {
        return NULL;
    }
    if (test_alloc_should_fail(uc, UC_TEST_ALLOC_FAIL_MAPPING_RECORD)) {
        return NULL;
    }
    mapping = g_try_malloc0(sizeof(*mapping));
    if (!mapping) {
        return NULL;
    }
    mapping->owner = uc;
    mapping->begin = begin;
    mapping->size = size;
    mapping->priority = uc->snapshot_level;
    return mapping;
}

static void mem_map_activate(uc_engine *uc, UcMapping *mapping)
{
    int pos;

    assert(mapping->owner == uc);
    assert(!mapping->active);
    assert(uc->mapped_block_count < uc->mapped_block_capacity);

    pos = bsearch_mapped_blocks(uc, mapping->begin);

    memmove(&uc->mapped_blocks[pos + 1], &uc->mapped_blocks[pos],
            sizeof(*uc->mapped_blocks) * (uc->mapped_block_count - pos));

    uc->mapped_blocks[pos] = mapping;
    uc->mapped_block_count++;
    mapping->active = true;
}

static void mem_map_insert(uc_engine *uc, UcMapping *mapping,
                           MemoryRegion *root)
{
    assert(root != NULL);
    mapping->root = root;
    mapping->regions = root;
    mapping->perms = root->perms;
    mapping->next = uc->mapping_records;
    uc->mapping_records = mapping;
    root->uc_mapping = mapping;
    root->mapping_offset = 0;
    mem_map_activate(uc, mapping);
}

static void mem_map_remove(uc_engine *uc, UcMapping *mapping)
{
    int pos = bsearch_mapped_blocks(uc, mapping->begin);

    assert(pos >= 0 && (uint32_t)pos < uc->mapped_block_count);
    assert(uc->mapped_blocks[pos] == mapping);
    uc->mapped_block_count--;
    memmove(&uc->mapped_blocks[pos], &uc->mapped_blocks[pos + 1],
            sizeof(*uc->mapped_blocks) * (uc->mapped_block_count - pos));
    mapping->active = false;
}

static void mapping_record_remove(uc_engine *uc, UcMapping *mapping)
{
    UcMapping **link = &uc->mapping_records;

    while (*link && *link != mapping) {
        link = &(*link)->next;
    }
    assert(*link == mapping);
    *link = mapping->next;
    mapping->next = NULL;
}

static void mapping_reclaim(uc_engine *uc, UcMapping *mapping)
{
    if (mapping->active) {
        return;
    }

    uc->memory_mapping_prune(mapping);
    if (mapping->context_refs != 0) {
        return;
    }
    mapping_record_remove(uc, mapping);
    uc->memory_mapping_free(mapping);
    g_free(mapping);
}

static void mapping_reclaim_inactive(uc_engine *uc)
{
    UcMapping *mapping = uc->mapping_records;

    while (mapping) {
        UcMapping *next = mapping->next;

        if (!mapping->active && mapping->context_refs == 0) {
            mapping_reclaim(uc, mapping);
        } else {
            uc->memory_mapping_prune(mapping);
        }
        mapping = next;
    }
}

static void mapping_normalize_active(uc_engine *uc)
{
    UcMapping *mapping;

    for (mapping = uc->mapping_records; mapping; mapping = mapping->next) {
        if (mapping->active) {
            uc->memory_mapping_normalize(mapping);
        }
    }
}

static uc_err mem_map_check(uc_engine *uc, uint64_t address, uint64_t size,
                            uint32_t perms)
{
    if (size == 0) {
        // invalid memory mapping
        return UC_ERR_ARG;
    }

    // address cannot wrap around
    if (address + size - 1 < address) {
        return UC_ERR_ARG;
    }

    // address must be aligned to uc->target_page_size
    if ((address & uc->target_page_align) != 0) {
        return UC_ERR_ARG;
    }

    // size must be multiple of uc->target_page_size
    if ((size & uc->target_page_align) != 0) {
        return UC_ERR_ARG;
    }

    // check for only valid permissions
    if ((perms & ~UC_PROT_ALL) != 0) {
        return UC_ERR_ARG;
    }

    // this area overlaps existing mapped regions?
    if (memory_overlap(uc, address, size)) {
        return UC_ERR_MAP;
    }

    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_mem_map(uc_engine *uc, uint64_t address, uint64_t size,
                  uint32_t perms)
{
    MemoryRegion *root;
    UcMapping *mapping;
    uc_err res;

    UC_INIT(uc);

    res = mem_map_check(uc, address, size, perms);
    if (res) {
        restore_jit_state(uc);
        return res;
    }

    mapping = mem_map_prepare(uc, address, size);
    if (!mapping) {
        restore_jit_state(uc);
        return UC_ERR_NOMEM;
    }
    root = uc->memory_map(uc, address, size, perms);
    if (!root) {
        g_free(mapping);
        restore_jit_state(uc);
        return UC_ERR_NOMEM;
    }
    mem_map_insert(uc, mapping, root);
    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_mem_map_ptr(uc_engine *uc, uint64_t address, uint64_t size,
                      uint32_t perms, void *ptr)
{
    MemoryRegion *root;
    UcMapping *mapping;
    uc_err res;

    UC_INIT(uc);

    if (ptr == NULL) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    res = mem_map_check(uc, address, size, perms);
    if (res) {
        restore_jit_state(uc);
        return res;
    }

    mapping = mem_map_prepare(uc, address, size);
    if (!mapping) {
        restore_jit_state(uc);
        return UC_ERR_NOMEM;
    }
    root = uc->memory_map_ptr(uc, address, size, perms, ptr);
    if (!root) {
        g_free(mapping);
        restore_jit_state(uc);
        return UC_ERR_NOMEM;
    }
    mem_map_insert(uc, mapping, root);
    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_mmio_map(uc_engine *uc, uint64_t address, uint64_t size,
                   uc_cb_mmio_read_t read_cb, void *user_data_read,
                   uc_cb_mmio_write_t write_cb, void *user_data_write)
{
    MemoryRegion *root;
    UcMapping *mapping;
    uc_err res;

    UC_INIT(uc);

    res = mem_map_check(uc, address, size, UC_PROT_ALL);
    if (res) {
        restore_jit_state(uc);
        return res;
    }

    mapping = mem_map_prepare(uc, address, size);
    if (!mapping) {
        restore_jit_state(uc);
        return UC_ERR_NOMEM;
    }

    /* Callback presence determines the installed MMIO permissions. */
    root = uc->memory_map_io(uc, address, size, read_cb, write_cb,
                             user_data_read, user_data_write);
    if (!root) {
        g_free(mapping);
        restore_jit_state(uc);
        return UC_ERR_NOMEM;
    }
    mem_map_insert(uc, mapping, root);
    restore_jit_state(uc);
    return UC_ERR_OK;
}

// Create a backup copy of the indicated MemoryRegion.
// Generally used in prepartion for splitting a MemoryRegion.
static uint8_t *copy_region(struct uc_struct *uc, MemoryRegion *mr)
{
    uint8_t *block = (uint8_t *)g_malloc0((uint64_t)int128_get64(mr->size));
    if (block != NULL) {
        uc_err err =
            uc_mem_read(uc, mr->addr, block, (uint64_t)int128_get64(mr->size));
        if (err != UC_ERR_OK) {
            free(block);
            block = NULL;
        }
    }

    return block;
}

static bool copy_mte_tags(RAMBlock *block, uint8_t **tags, ram_addr_t *tag_size)
{
    *tags = NULL;
    *tag_size = 0;
    if (block->mte_tags == NULL || block->mte_tags_size == 0) {
        return true;
    }

    *tags = g_malloc0(block->mte_tags_size);
    if (*tags == NULL) {
        return false;
    }

    memcpy(*tags, block->mte_tags, block->mte_tags_size);
    *tag_size = block->mte_tags_size;
    return true;
}

static bool restore_mte_tags(struct uc_struct *uc, uint64_t address,
                             uint64_t size, uint64_t source_offset,
                             const uint8_t *tags, ram_addr_t tag_size)
{
    MemoryRegion *mr;
    RAMBlock *block;
    ram_addr_t source_tag_offset;
    ram_addr_t copy_size;

    if (tags == NULL || size == 0) {
        return true;
    }

    source_tag_offset = source_offset / UC_MTE_TAG_STORAGE_GRANULE;
    if (source_tag_offset >= tag_size) {
        return true;
    }

    mr = uc->memory_mapping(uc, address);
    if (mr == NULL || !mr->ram || mr->ram_block == NULL) {
        return false;
    }

    block = mr->ram_block;
    if (block->mte_tags == NULL) {
        block->mte_tags_size =
            (block->max_length + UC_MTE_TAG_STORAGE_GRANULE - 1) /
            UC_MTE_TAG_STORAGE_GRANULE;
        block->mte_tags = g_malloc0(block->mte_tags_size);
        if (block->mte_tags == NULL) {
            block->mte_tags_size = 0;
            return false;
        }
    }

    copy_size =
        (size + UC_MTE_TAG_STORAGE_GRANULE - 1) / UC_MTE_TAG_STORAGE_GRANULE;
    copy_size = MIN(copy_size, tag_size - source_tag_offset);
    copy_size = MIN(copy_size, block->mte_tags_size);
    memcpy(block->mte_tags, tags + source_tag_offset, copy_size);
    return true;
}

static bool split_region_layout(uint64_t begin, uint64_t region_size,
                                uint64_t address, uint64_t size,
                                uint64_t *left_size, uint64_t *middle_size,
                                uint64_t *right_size)
{
    uint64_t offset;

    /* Do not form an exclusive end: terminal mappings wrap it to zero. */
    if (address < begin) {
        offset = begin - address;
        if (size <= offset) {
            return false;
        }
        size -= offset;
        offset = 0;
    } else {
        offset = address - begin;
        if (offset >= region_size) {
            return false;
        }
    }

    *left_size = offset;
    *middle_size = MIN(size, region_size - offset);
    *right_size = region_size - offset - *middle_size;
    return true;
}

/*
    This function is similar to split_region, but for MMIO memory.

    Note this function may be called recursively.
*/
static bool split_mmio_region(struct uc_struct *uc, MemoryRegion *mr,
                              uint64_t address, uint64_t size, bool do_delete)
{
    uint64_t begin, middle_begin;
    uint64_t region_size;
    uint64_t l_size, r_size, m_size;
    mmio_cbs backup;

    if (size == 0) {
        return false;
    }

    begin = mr->addr;
    region_size = (uint64_t)int128_get64(mr->size);
    if (!split_region_layout(begin, region_size, address, size, &l_size,
                             &m_size, &r_size)) {
        return false;
    }

    // This branch also breaks recursion.
    if (l_size == 0 && r_size == 0) {
        return true;
    }

    middle_begin = begin + l_size;

    memcpy(&backup, mr->opaque, sizeof(mmio_cbs));

    /* overlapping cases
     *               |------mr------|
     * case 1    |---size--|            // Is it possible???
     * case 2           |--size--|
     * case 3                  |---size--|
     */

    // unmap this region first, then do split it later
    if (uc_mem_unmap(uc, mr->addr, (uint64_t)int128_get64(mr->size)) !=
        UC_ERR_OK) {
        return false;
    }

    if (l_size > 0) {
        if (uc_mmio_map(uc, begin, l_size, backup.read, backup.user_data_read,
                        backup.write, backup.user_data_write) != UC_ERR_OK) {
            return false;
        }
    }

    if (m_size > 0 && !do_delete) {
        if (uc_mmio_map(uc, middle_begin, m_size, backup.read,
                        backup.user_data_read, backup.write,
                        backup.user_data_write) != UC_ERR_OK) {
            return false;
        }
    }

    if (r_size > 0) {
        uint64_t right_begin = middle_begin + m_size;

        if (uc_mmio_map(uc, right_begin, r_size, backup.read,
                        backup.user_data_read, backup.write,
                        backup.user_data_write) != UC_ERR_OK) {
            return false;
        }
    }

    return true;
}

/*
   Split the given MemoryRegion at the indicated address for the indicated size
   this may result in the create of up to 3 spanning sections. If the delete
   parameter is true, the no new section will be created to replace the indicate
   range. This functions exists to support uc_mem_protect and uc_mem_unmap.

   This is a static function and callers have already done some preliminary
   parameter validation.

   The do_delete argument indicates that we are being called to support
   uc_mem_unmap. In this case we save some time by choosing NOT to remap
   the areas that are intended to get unmapped
 */
// TODO: investigate whether qemu region manipulation functions already offered
// this capability
static bool split_region(struct uc_struct *uc, MemoryRegion *mr,
                         uint64_t address, uint64_t size, bool do_delete)
{
    uint8_t *backup;
    uint32_t perms;
    uint64_t begin, middle_begin;
    uint64_t region_size;
    uint64_t l_size, m_size, r_size;
    RAMBlock *block = NULL;
    bool prealloc = false;
    uint8_t *tag_backup = NULL;
    ram_addr_t tag_backup_size = 0;

    if (size == 0) {
        // trivial case
        return true;
    }

    begin = mr->addr;
    region_size = (uint64_t)int128_get64(mr->size);
    if (!split_region_layout(begin, region_size, address, size, &l_size,
                             &m_size, &r_size)) {
        return false;
    }

    // if this region belongs to the requested area, there is no work to do.
    if (l_size == 0 && r_size == 0) {
        return true;
    }

    middle_begin = begin + l_size;

    // Find the correct and large enough (which contains our target mr)
    // to create the content backup.
    block = mr->ram_block;

    if (block == NULL) {
        return false;
    }

    // RAM_PREALLOC is not defined outside exec.c and I didn't feel like
    // moving it
    prealloc = !!(block->flags & 1);

    if (block->flags & 1) {
        backup = block->host;
    } else {
        backup = copy_region(uc, mr);
        if (backup == NULL) {
            return false;
        }
    }
    if (!copy_mte_tags(block, &tag_backup, &tag_backup_size)) {
        goto error;
    }

    // save the essential information required for the split before mr gets
    // deleted
    perms = mr->perms;

    // unmap this region first, then do split it later
    if (uc_mem_unmap(uc, mr->addr, (uint64_t)int128_get64(mr->size)) !=
        UC_ERR_OK) {
        goto error;
    }

    /* overlapping cases
     *               |------mr------|
     * case 1    |---size--|
     * case 2           |--size--|
     * case 3                  |---size--|
     */

    // If there are error in any of the below operations, things are too far
    // gone at that point to recover. Could try to remap orignal region, but
    // these smaller allocation just failed so no guarantee that we can recover
    // the original allocation at this point
    if (l_size > 0) {
        if (!prealloc) {
            if (uc_mem_map(uc, begin, l_size, perms) != UC_ERR_OK) {
                goto error;
            }
            if (uc_mem_write(uc, begin, backup, l_size) != UC_ERR_OK) {
                goto error;
            }
        } else {
            if (uc_mem_map_ptr(uc, begin, l_size, perms, backup) != UC_ERR_OK) {
                goto error;
            }
        }
        if (!restore_mte_tags(uc, begin, l_size, 0, tag_backup,
                              tag_backup_size)) {
            goto error;
        }
    }

    if (m_size > 0 && !do_delete) {
        if (!prealloc) {
            if (uc_mem_map(uc, middle_begin, m_size, perms) != UC_ERR_OK) {
                goto error;
            }
            if (uc_mem_write(uc, middle_begin, backup + l_size, m_size) !=
                UC_ERR_OK) {
                goto error;
            }
        } else {
            if (uc_mem_map_ptr(uc, middle_begin, m_size, perms,
                               backup + l_size) != UC_ERR_OK) {
                goto error;
            }
        }
        if (!restore_mte_tags(uc, middle_begin, m_size, l_size, tag_backup,
                              tag_backup_size)) {
            goto error;
        }
    }

    if (r_size > 0) {
        uint64_t right_begin = middle_begin + m_size;

        if (!prealloc) {
            if (uc_mem_map(uc, right_begin, r_size, perms) != UC_ERR_OK) {
                goto error;
            }
            if (uc_mem_write(uc, right_begin, backup + l_size + m_size,
                             r_size) != UC_ERR_OK) {
                goto error;
            }
        } else {
            if (uc_mem_map_ptr(uc, right_begin, r_size, perms,
                               backup + l_size + m_size) != UC_ERR_OK) {
                goto error;
            }
        }
        if (!restore_mte_tags(uc, right_begin, r_size, l_size + m_size,
                              tag_backup, tag_backup_size)) {
            goto error;
        }
    }

    g_free(tag_backup);
    if (!prealloc) {
        free(backup);
    }
    return true;

error:
    g_free(tag_backup);
    if (!prealloc) {
        free(backup);
    }
    return false;
}

UNICORN_EXPORT
uc_err uc_mem_protect(struct uc_struct *uc, uint64_t address, uint64_t size,
                      uint32_t perms)
{
    MemoryRegion *mr;
    uint64_t addr = address;
    uint64_t pc;
    uint64_t count, len;
    bool remove_exec = false;

    UC_INIT(uc);

    // snapshot and protection can't be mixed
    if (uc->memory_context_count != 0) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    if (size == 0) {
        // trivial case, no change
        restore_jit_state(uc);
        return UC_ERR_OK;
    }

    // address must be aligned to uc->target_page_size
    if ((address & uc->target_page_align) != 0) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    // size must be multiple of uc->target_page_size
    if ((size & uc->target_page_align) != 0) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    // check for only valid permissions
    if ((perms & ~UC_PROT_ALL) != 0) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    // check that user's entire requested block is mapped
    // TODO check if protected is possible
    // deny after cow
    if (!check_mem_area(uc, address, size, NULL)) {
        restore_jit_state(uc);
        return UC_ERR_NOMEM;
    }

    // Now we know entire region is mapped, so change permissions
    // We may need to split regions if this area spans adjacent regions
    addr = address;
    count = 0;
    while (count < size) {
        mr = uc->memory_mapping(uc, addr);
        len = memory_region_len(uc, mr, addr, size - count);
        if (mr->ram) {
            if (!split_region(uc, mr, addr, len, false)) {
                restore_jit_state(uc);
                return UC_ERR_NOMEM;
            }

            mr = uc->memory_mapping(uc, addr);
            // will this remove EXEC permission?
            if (((mr->perms & UC_PROT_EXEC) != 0) &&
                ((perms & UC_PROT_EXEC) == 0)) {
                remove_exec = true;
            }
            mr->perms = perms;
            mr->uc_mapping->perms = perms;
            uc->readonly_mem(mr, (perms & UC_PROT_WRITE) == 0);

        } else {
            if (!split_mmio_region(uc, mr, addr, len, false)) {
                restore_jit_state(uc);
                return UC_ERR_NOMEM;
            }

            mr = uc->memory_mapping(uc, addr);
            mr->perms = perms;
            mr->uc_mapping->perms = perms;
        }

        count += len;
        addr += len;
    }

    // if EXEC permission is removed, then quit TB and continue at the same
    // place
    if (remove_exec && uc->nested_level != 0) {
        pc = uc->get_pc(uc);
        if (pc >= address && pc - address < size) {
            uc->quit_request = true;
            uc_emu_stop(uc);
        }
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

static uc_err uc_mem_unmap_snapshot(struct uc_struct *uc, uint64_t address,
                                    uint64_t size)
{
    UcMapping *mapping;

    mapping = mapped_block_at(uc, address);
    if (!mapping || mapping->begin != address || mapping->size != size) {
        return UC_ERR_ARG;
    }

    uc->memory_unmap(uc, mapping);
    mem_map_remove(uc, mapping);
    mapping_reclaim(uc, mapping);

    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_mem_unmap(struct uc_struct *uc, uint64_t address, uint64_t size)
{
    MemoryRegion *mr;
    uint64_t addr;
    uint64_t pc;
    uint64_t count, len;

    UC_INIT(uc);

    if (size == 0) {
        // nothing to unmap
        restore_jit_state(uc);
        return UC_ERR_OK;
    }

    // address must be aligned to uc->target_page_size
    if ((address & uc->target_page_align) != 0) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    // size must be multiple of uc->target_page_size
    if ((size & uc->target_page_align) != 0) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    // check that user's entire requested block is mapped
    if (!check_mem_area(uc, address, size, NULL)) {
        restore_jit_state(uc);
        return UC_ERR_NOMEM;
    }

    if (uc->memory_context_count != 0) {
        uc_err res = uc_mem_unmap_snapshot(uc, address, size);
        restore_jit_state(uc);
        return res;
    }

    pc = uc->get_pc(uc);

    // Now we know entire region is mapped, so do the unmap
    // We may need to split regions if this area spans adjacent regions
    addr = address;
    count = 0;
    while (count < size) {
        mr = uc->memory_mapping(uc, addr);
        len = memory_region_len(uc, mr, addr, size - count);
        if (!mr->ram) {
            if (!split_mmio_region(uc, mr, addr, len, true)) {
                restore_jit_state(uc);
                return UC_ERR_NOMEM;
            }
        } else {
            if (!split_region(uc, mr, addr, len, true)) {
                restore_jit_state(uc);
                return UC_ERR_NOMEM;
            }
        }

        // if we can retrieve the mapping, then no splitting took place
        // so unmap here
        {
            UcMapping *mapping = mapped_block_at(uc, addr);

            if (mapping != NULL) {
                uc->memory_unmap(uc, mapping);
                mem_map_remove(uc, mapping);
                mapping_reclaim(uc, mapping);
            }
        }
        count += len;
        addr += len;
    }

    if (uc->nested_level != 0 && pc >= address && pc - address < size) {
        uc->quit_request = true;
        uc_emu_stop(uc);
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_hook_add(uc_engine *uc, uc_hook *hh, int type, void *callback,
                   void *user_data, uint64_t begin, uint64_t end, ...)
{
    const unsigned int valid_types = (1U << UC_HOOK_MAX) - 1;
    int ret = UC_ERR_OK;
    int i = 0;

    UC_INIT(uc);

    if (type == 0 || ((unsigned int)type & ~valid_types) != 0) {
        restore_jit_state(uc);
        return UC_ERR_HOOK;
    }

    struct hook *hook = calloc(1, sizeof(struct hook));
    if (hook == NULL) {
        restore_jit_state(uc);
        return UC_ERR_NOMEM;
    }

    hook->begin = begin;
    hook->end = end;
    hook->type = type;
    hook->callback = callback;
    hook->user_data = user_data;
    hook->refs = 0;
    hook->to_delete = false;
    hook->hooked_regions = g_hash_table_new_full(
        hooked_regions_hash, hooked_regions_equal, g_free, NULL);
    *hh = (uc_hook)hook;

    // UC_HOOK_INSN has an extra argument for instruction ID
    if (type & UC_HOOK_INSN) {
        va_list valist;

        va_start(valist, end);
        hook->insn = va_arg(valist, int);
        va_end(valist);

        if (uc->insn_hook_validate) {
            if (!uc->insn_hook_validate(hook->insn)) {
                free(hook);
                restore_jit_state(uc);
                return UC_ERR_HOOK;
            }
        }

        if (uc->hook_insert) {
            if (hook_insert(&uc->hook[UC_HOOK_INSN_IDX], hook) == NULL) {
                free(hook);
                restore_jit_state(uc);
                return UC_ERR_NOMEM;
            }
        } else {
            if (hook_append(&uc->hook[UC_HOOK_INSN_IDX], hook) == NULL) {
                free(hook);
                restore_jit_state(uc);
                return UC_ERR_NOMEM;
            }
        }

        uc->hooks_count[UC_HOOK_INSN_IDX]++;
        hook_dispatch_cache_invalidate(uc, UC_HOOK_INSN_IDX);
        if (uc->arch == UC_ARCH_ARM64) {
            request_tb_flush(uc);
            if (uc->nested_level) {
                uc->quit_request = true;
                break_translation_loop(uc);
            }
        }
        restore_jit_state(uc);
        return UC_ERR_OK;
    }

    if (type & UC_HOOK_TCG_OPCODE) {
        va_list valist;

        va_start(valist, end);
        hook->op = va_arg(valist, int);
        hook->op_flags = va_arg(valist, int);
        va_end(valist);

        if (uc->opcode_hook_invalidate) {
            if (!uc->opcode_hook_invalidate(hook->op, hook->op_flags)) {
                free(hook);
                restore_jit_state(uc);
                return UC_ERR_HOOK;
            }
        }

        if (uc->hook_insert) {
            if (hook_insert(&uc->hook[UC_HOOK_TCG_OPCODE_IDX], hook) == NULL) {
                free(hook);
                restore_jit_state(uc);
                return UC_ERR_NOMEM;
            }
        } else {
            if (hook_append(&uc->hook[UC_HOOK_TCG_OPCODE_IDX], hook) == NULL) {
                free(hook);
                restore_jit_state(uc);
                return UC_ERR_NOMEM;
            }
        }

        uc->hooks_count[UC_HOOK_TCG_OPCODE_IDX]++;
        hook_dispatch_cache_invalidate(uc, UC_HOOK_TCG_OPCODE_IDX);
        return UC_ERR_OK;
    }

    if (type & (UC_HOOK_CODE | UC_HOOK_BLOCK | UC_HOOK_MEM_FETCH)) {
        hook_invalidate_range(uc, begin, end);
        if (uc->nested_level) {
            uc->quit_request = true;
            break_translation_loop(uc);
        }
    }

    while ((type >> i) > 0) {
        if ((type >> i) & 1) {
            // TODO: invalid hook error?
            if (i < UC_HOOK_MAX) {
                if (uc->hook_insert) {
                    if (hook_insert(&uc->hook[i], hook) == NULL) {
                        free(hook);
                        restore_jit_state(uc);
                        return UC_ERR_NOMEM;
                    }
                } else {
                    if (hook_append(&uc->hook[i], hook) == NULL) {
                        free(hook);
                        restore_jit_state(uc);
                        return UC_ERR_NOMEM;
                    }
                }
                uc->hooks_count[i]++;
                hook_dispatch_cache_invalidate(uc, i);
                if (i == UC_HOOK_EDGE_GENERATED_IDX) {
                    uc->last_tb_valid = false;
                }
            }
        }
        i++;
    }

    if (hook->refs != 0 && (type & UC_HOOK_MEM_FAST_PATH)) {
        uc->tcg_flush_tlb(uc);
    }

    // we didn't use the hook
    // TODO: return an error?
    if (hook->refs == 0) {
        free(hook);
    }

    restore_jit_state(uc);
    return ret;
}

UNICORN_EXPORT
uc_err uc_hook_del(uc_engine *uc, uc_hook hh)
{
    int i;
    bool flush_tlb = false;
    bool flush_tb = false;
    bool exit_direct_hook = false;
    struct hook *hook = (struct hook *)hh;

    UC_INIT(uc);

    // we can't dereference hook->type if hook is invalid
    // so for now we need to iterate over all possible types to remove the hook
    // which is less efficient
    // an optimization would be to align the hook pointer
    // and store the type mask in the hook pointer.
    for (i = 0; i < UC_HOOK_MAX; i++) {
        if (list_exists(&uc->hook[i], (void *)hook)) {
            if (hook->type & UC_HOOK_MEM_FAST_PATH) {
                flush_tlb = true;
            }
            if (i == UC_HOOK_INSN_IDX && uc->arch == UC_ARCH_ARM64) {
                flush_tb = true;
            }
            if (hook->type &
                (UC_HOOK_CODE | UC_HOOK_BLOCK | UC_HOOK_MEM_FETCH)) {
                g_hash_table_foreach(hook->hooked_regions,
                                     hook_invalidate_region, uc);
                if ((i == UC_HOOK_CODE_IDX || i == UC_HOOK_BLOCK_IDX) &&
                    uc->hooks_count[i] == 1 &&
                    (uc_hook)hook != uc->count_hook) {
                    exit_direct_hook = true;
                }
            }
            g_hash_table_remove_all(hook->hooked_regions);
            hook->to_delete = true;
            uc->hooks_count[i]--;
            hook_dispatch_cache_invalidate(uc, i);
            if (i == UC_HOOK_EDGE_GENERATED_IDX) {
                uc->last_tb_valid = false;
            }
            hook_append(&uc->hooks_to_del, hook);
        }
    }

    if (flush_tlb) {
        uc->tcg_flush_tlb(uc);
    }
    if (flush_tb) {
        request_tb_flush(uc);
        if (uc->nested_level) {
            uc->quit_request = true;
            break_translation_loop(uc);
        }
    }
    if (exit_direct_hook && uc->nested_level) {
        uc->quit_request = true;
        break_translation_loop(uc);
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_hook_set_user_data(uc_engine *uc, uc_hook hh, void *user_data)
{
    struct hook *hook = (struct hook *)hh;
    if (hook->type & (UC_HOOK_BLOCK | UC_HOOK_CODE | UC_HOOK_MEM_FETCH)) {
        if (uc->nested_level) {
            return UC_ERR_ARG;
        }
        hook_invalidate_range(uc, hook->begin, hook->end);
    }
    hook->user_data = user_data;
    return UC_ERR_OK;
}

// TCG helper
// 2 arguments are enough for most opcodes. Load/Store needs 3 arguments but we
// have memory hooks already. We may exceed the maximum arguments of a tcg
// helper but that's easy to extend.
void helper_uc_traceopcode(struct hook *hook, uint64_t arg1, uint64_t arg2,
                           uint32_t size, void *handle, uint64_t address);
void helper_uc_traceopcode(struct hook *hook, uint64_t arg1, uint64_t arg2,
                           uint32_t size, void *handle, uint64_t address)
{
    struct uc_struct *uc = handle;

    if (unlikely(uc_stop_requested(uc))) {
        return;
    }

    if (unlikely(hook->to_delete)) {
        return;
    }

    // We did all checks in translation time.
    //
    // This could optimize the case that we have multiple hooks with different
    // opcodes and have one callback per opcode. Note that the assumption don't
    // hold in most cases for uc_tracecode.
    //
    // TODO: Shall we have a flag to allow users to control whether updating PC?
    JIT_CALLBACK_GUARD(((uc_hook_tcg_op_2)hook->callback)(
        uc, address, arg1, arg2, size, hook->user_data));

    if (unlikely(uc_stop_requested(uc))) {
        return;
    }
}

void helper_uc_tracecode(int32_t size, uc_hook_idx index, void *handle,
                         int64_t address);
void helper_uc_tracecode(int32_t size, uc_hook_idx index, void *handle,
                         int64_t address)
{
    struct uc_struct *uc = handle;
    struct list_item *cur;
    struct hook *hook;
    int hook_flags =
        index &
        UC_HOOK_FLAG_MASK; // The index here may contain additional flags. See
                           // the comments of uc_hook_idx for details.
    // bool not_allow_stop = (size & UC_HOOK_FLAG_NO_STOP) || (hook_flags &
    // UC_HOOK_FLAG_NO_STOP);
    bool not_allow_stop = hook_flags & UC_HOOK_FLAG_NO_STOP;

    index = index & UC_HOOK_IDX_MASK;
    // // Like hook index, only low 6 bits of size is used for representing
    // sizes. size = size & UC_HOOK_IDX_MASK;

    // This has been done in tcg code.
    // sync PC in CPUArchState with address
    // if (uc->set_pc) {
    //     uc->set_pc(uc, address);
    // }

    // the last callback may already asked to stop emulation
    if (uc_stop_requested(uc) && !not_allow_stop) {
        return;
    } else if (not_allow_stop && uc_stop_requested(uc)) {
        revert_uc_emu_stop(uc);
    }

    if (index == UC_HOOK_MEM_FETCH_IDX) {
        for (cur = uc->hook[index].head;
             cur != NULL && (hook = (struct hook *)cur->data);
             cur = cur->next) {
            if (hook->to_delete) {
                continue;
            }
            if (HOOK_BOUND_CHECK(hook, (uint64_t)address)) {
                JIT_CALLBACK_GUARD(((uc_cb_hookmem_t)hook->callback)(
                    uc, UC_MEM_FETCH, address, size, 0, hook->user_data));
            }
            if (not_allow_stop && uc_stop_requested(uc)) {
                revert_uc_emu_stop(uc);
            } else if (!not_allow_stop && uc_stop_requested(uc)) {
                return;
            }
        }
        return;
    }

    for (cur = uc->hook[index].head;
         cur != NULL && (hook = (struct hook *)cur->data); cur = cur->next) {
        if (hook->to_delete) {
            continue;
        }

        // on invalid block/instruction, call instruction counter (if enable),
        // then quit
        if (size == 0) {
            if (index == UC_HOOK_CODE_IDX && uc->count_hook) {
                // this is the instruction counter (first hook in the list)
                JIT_CALLBACK_GUARD(((uc_cb_hookcode_t)hook->callback)(
                    uc, address, size, hook->user_data));
            }

            return;
        }

        if (HOOK_BOUND_CHECK(hook, (uint64_t)address)) {
            JIT_CALLBACK_GUARD(((uc_cb_hookcode_t)hook->callback)(
                uc, address, size, hook->user_data));
        }

        // the last callback may already asked to stop emulation
        // Unicorn:
        //   In an ARM IT block, we behave like the emulation continues
        //   normally. No check_exit_request is generated and the hooks are
        //   triggered normally. In other words, the whole IT block is treated
        //   as a single instruction.
        if (not_allow_stop && uc_stop_requested(uc)) {
            revert_uc_emu_stop(uc);
        } else if (!not_allow_stop && uc_stop_requested(uc)) {
            break;
        }
    }
}

UNICORN_EXPORT
uc_err uc_mem_regions(uc_engine *uc, uc_mem_region **regions, uint32_t *count)
{
    uint32_t i;
    uc_mem_region *r = NULL;

    UC_INIT(uc);

    *count = uc->mapped_block_count;

    if (*count) {
        r = g_malloc0(*count * sizeof(uc_mem_region));
        if (r == NULL) {
            // out of memory
            restore_jit_state(uc);
            return UC_ERR_NOMEM;
        }
    }

    for (i = 0; i < *count; i++) {
        r[i].begin = uc->mapped_blocks[i]->begin;
        r[i].end =
            uc->mapped_blocks[i]->begin + uc->mapped_blocks[i]->size - 1;
        r[i].perms = uc->mapped_blocks[i]->perms;
    }

    *regions = r;

    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_query(uc_engine *uc, uc_query_type type, size_t *result)
{
    UC_INIT(uc);

    switch (type) {
    default:
        restore_jit_state(uc);
        return UC_ERR_ARG;

    case UC_QUERY_PAGE_SIZE:
        *result = uc->target_page_size;
        break;

    case UC_QUERY_ARCH:
        *result = uc->arch;
        break;

    case UC_QUERY_MODE:
#ifdef UNICORN_HAS_ARM
        if (uc->arch == UC_ARCH_ARM) {
            uc_err ret = uc->query(uc, type, result);

            restore_jit_state(uc);
            return ret;
        }
#endif
        *result = uc->mode;
        break;

    case UC_QUERY_TIMEOUT:
        *result = emu_atomic_read(&uc->timed_out);
        break;
    }

    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_context_alloc(uc_engine *uc, uc_context **context)
{
    /* QEMU target CPU states containing vector registers require alignment. */
    const size_t alignment = 16;
    struct uc_context **_context = context;
    size_t size = uc_context_size(uc);
    UcContextAllocation *allocation;
    uintptr_t data_address;

    UC_INIT(uc);

    allocation = g_malloc(sizeof(*allocation) + size + alignment - 1);
    if (allocation) {
        data_address = ((uintptr_t)(allocation + 1) +
                        sizeof(uc_context) + alignment - 1) &
                       ~(uintptr_t)(alignment - 1);
        *_context = (uc_context *)(data_address - sizeof(uc_context));
        memset(*_context, 0, size);
        (*_context)->context_size = size - sizeof(uc_context);
        (*_context)->arch = uc->arch;
        (*_context)->mode = uc->mode;
        allocation->capacity = (*_context)->context_size;
        allocation->context = *_context;
        while (emu_atomic_cmpxchg(&context_allocations_lock, 0, 1) != 0) {
        }
        allocation->next = context_allocations;
        context_allocations = allocation;
        emu_atomic_set(&context_allocations_lock, 0);
        restore_jit_state(uc);
        return UC_ERR_OK;
    } else {
        restore_jit_state(uc);
        return UC_ERR_NOMEM;
    }
}

UNICORN_EXPORT
uc_err uc_free(void *mem)
{
    g_free(mem);
    return UC_ERR_OK;
}

static size_t context_data_size(uc_engine *uc)
{
    if (!uc->context_size) {
        return uc->cpu_context_size;
    }
    return uc->context_size(uc);
}

static bool context_matches_arch_mode(uc_engine *uc,
                                      const uc_context *context)
{
    return context && context->arch == uc->arch && context->mode == uc->mode;
}

static bool context_allocation_capacity(const uc_context *context,
                                        size_t *capacity)
{
    UcContextAllocation *allocation = NULL;

    while (emu_atomic_cmpxchg(&context_allocations_lock, 0, 1) != 0) {
    }
    for (allocation = context_allocations; allocation;
         allocation = allocation->next) {
        if (allocation->context == context) {
            *capacity = allocation->capacity;
            break;
        }
    }
    emu_atomic_set(&context_allocations_lock, 0);
    return allocation != NULL;
}

UNICORN_EXPORT
size_t uc_context_size(uc_engine *uc)
{
    size_t size;

    UC_INIT(uc);

    size = sizeof(uc_context) + context_data_size(uc);
    restore_jit_state(uc);
    return size;
}

static void context_memory_register(uc_engine *uc, uc_context *context)
{
    assert(!context->memory_owner);
    assert(!context->memory_prev);
    assert(!context->memory_next);

    context->memory_owner = uc;
    context->memory_next = uc->memory_contexts;
    if (context->memory_next) {
        context->memory_next->memory_prev = context;
    }
    uc->memory_contexts = context;
    uc->memory_context_count++;
}

static void context_memory_unregister(uc_context *context)
{
    uc_engine *uc = context->memory_owner;

    assert(uc);
    assert(uc->memory_context_count != 0);
    if (context->memory_prev) {
        context->memory_prev->memory_next = context->memory_next;
    } else {
        assert(uc->memory_contexts == context);
        uc->memory_contexts = context->memory_next;
    }
    if (context->memory_next) {
        context->memory_next->memory_prev = context->memory_prev;
    }
    context->memory_owner = NULL;
    context->memory_prev = NULL;
    context->memory_next = NULL;
    uc->memory_context_count--;
}

static void context_memory_clear(uc_context *context, bool reclaim,
                                 bool normalize)
{
    uc_engine *uc = context->memory_owner;
    uint32_t i;

    if (uc) {
        context_memory_unregister(context);
    }
    if (context->fv) {
        g_free(context->fv->ranges);
        g_free(context->fv);
    }

    if (uc) {
        for (i = 0; i < context->memory_region_count; i++) {
            MemoryRegion *region = context->memory_regions[i];

            assert(region->context_refs != 0);
            region->context_refs--;
        }
        for (i = 0; i < context->mapping_count; i++) {
            UcMapping *mapping = context->mappings[i].mapping;

            assert(mapping->context_refs != 0);
            mapping->context_refs--;
        }
        if (reclaim) {
            if (normalize && uc->memory_context_count == 0) {
                mapping_normalize_active(uc);
            }
            mapping_reclaim_inactive(uc);
            if (uc->memory_context_count == 0) {
                uc->tcg_flush_tlb(uc);
            }
        }
    }

    g_free(context->mappings);
    g_free(context->memory_regions);
    context->fv = NULL;
    context->mappings = NULL;
    context->memory_regions = NULL;
    context->mapping_count = 0;
    context->memory_region_count = 0;
}

static bool context_mapping_refs_available(uc_engine *uc)
{
    uint32_t mapping_index;

    for (mapping_index = 0; mapping_index < uc->mapped_block_count;
         mapping_index++) {
        UcMapping *mapping = uc->mapped_blocks[mapping_index];
        MemoryRegion *region;

        if (mapping->context_refs == UINT32_MAX) {
            return false;
        }
        if (mapping->root->terminates) {
            if (mapping->root->context_refs == UINT32_MAX) {
                return false;
            }
        } else {
            QTAILQ_FOREACH(region, &mapping->root->subregions, subregions_link)
            {
                if (region->context_refs == UINT32_MAX) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool context_capture_mappings(uc_engine *uc,
                                     UcContextMapping **mappings_out,
                                     MemoryRegion ***regions_out,
                                     uint32_t *region_count_out)
{
    UcContextMapping *mappings = NULL;
    MemoryRegion **regions = NULL;
    uint64_t region_count = 0;
    uint32_t mapping_index;
    uint32_t region_index = 0;

    for (mapping_index = 0; mapping_index < uc->mapped_block_count;
         mapping_index++) {
        UcMapping *mapping = uc->mapped_blocks[mapping_index];
        MemoryRegion *region;

        if (mapping->root->terminates) {
            region_count++;
        } else {
            QTAILQ_FOREACH(region, &mapping->root->subregions, subregions_link)
            {
                region_count++;
            }
        }
        if (region_count > UINT32_MAX) {
            return false;
        }
    }

    if (uc->mapped_block_count != 0) {
        if (test_alloc_should_fail(uc,
                                   UC_TEST_ALLOC_FAIL_CONTEXT_MAPPINGS)) {
            return false;
        }
        mappings = g_try_new(UcContextMapping, uc->mapped_block_count);
        if (!mappings) {
            return false;
        }
    }
    if (region_count != 0) {
        if (test_alloc_should_fail(uc,
                                   UC_TEST_ALLOC_FAIL_CONTEXT_REGIONS)) {
            g_free(mappings);
            return false;
        }
        regions = g_try_new(MemoryRegion *, (uint32_t)region_count);
        if (!regions) {
            g_free(mappings);
            return false;
        }
    }

    for (mapping_index = 0; mapping_index < uc->mapped_block_count;
         mapping_index++) {
        UcMapping *mapping = uc->mapped_blocks[mapping_index];
        UcContextMapping *saved = &mappings[mapping_index];
        MemoryRegion *region;

        saved->mapping = mapping;
        saved->first_region = region_index;
        if (mapping->root->terminates) {
            regions[region_index++] = mapping->root;
        } else {
            QTAILQ_FOREACH(region, &mapping->root->subregions, subregions_link)
            {
                regions[region_index++] = region;
            }
        }
        saved->region_count = region_index - saved->first_region;
        assert(saved->region_count != 0);
    }

    assert(region_index == region_count);
    *mappings_out = mappings;
    *regions_out = regions;
    *region_count_out = region_index;
    return true;
}

UNICORN_EXPORT
uc_err uc_context_save(uc_engine *uc, uc_context *context)
{
    UC_INIT(uc);
    uc_err ret = UC_ERR_OK;
    size_t data_size = context_data_size(uc);
    size_t capacity = 0;
    bool allocated;

    if (!context) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }
    allocated = context_allocation_capacity(context, &capacity);
    if (allocated) {
        if (!context_matches_arch_mode(uc, context) || capacity < data_size) {
            restore_jit_state(uc);
            return UC_ERR_ARG;
        }
    } else {
        if (uc->context_content & UC_CTL_CONTEXT_MEMORY) {
            restore_jit_state(uc);
            return UC_ERR_ARG;
        }
        memset(context, 0, sizeof(*context));
        context->arch = uc->arch;
        context->mode = uc->mode;
    }
    if ((uc->context_content & UC_CTL_CONTEXT_MEMORY) &&
        context->memory_owner && context->memory_owner != uc) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }
    context->context_size = data_size;

    if (uc->context_content & UC_CTL_CONTEXT_MEMORY) {
        UcContextMapping *mappings = NULL;
        MemoryRegion **regions = NULL;
        uint32_t region_count = 0;
        FlatView *fv = NULL;

        if (!test_alloc_should_fail(uc, UC_TEST_ALLOC_FAIL_CONTEXT_VIEW)) {
            fv = g_try_malloc0(sizeof(*fv));
        }

        if (uc->memory_context_count == UINT32_MAX) {
            g_free(fv);
            restore_jit_state(uc);
            return UC_ERR_RESOURCE;
        }
        if (!context_mapping_refs_available(uc)) {
            g_free(fv);
            restore_jit_state(uc);
            return UC_ERR_RESOURCE;
        }
        if (!fv ||
            !context_capture_mappings(uc, &mappings, &regions, &region_count) ||
            !uc->flatview_copy(uc, fv, uc->address_space_memory.current_map,
                               false)) {
            if (fv) {
                g_free(fv->ranges);
            }
            g_free(fv);
            g_free(mappings);
            g_free(regions);
            restore_jit_state(uc);
            return UC_ERR_NOMEM;
        }
        ret = uc_snapshot(uc);
        if (ret != UC_ERR_OK) {
            g_free(fv->ranges);
            g_free(fv);
            g_free(mappings);
            g_free(regions);
            restore_jit_state(uc);
            return ret;
        }

        context_memory_clear(context, true, false);
        context->fv = fv;
        context->mappings = mappings;
        context->memory_regions = regions;
        context->mapping_count = uc->mapped_block_count;
        context->memory_region_count = region_count;
        for (uint32_t i = 0; i < context->mapping_count; i++) {
            context->mappings[i].mapping->context_refs++;
        }
        for (uint32_t i = 0; i < context->memory_region_count; i++) {
            context->memory_regions[i]->context_refs++;
        }
        context_memory_register(uc, context);
        context->ramblock_freed = uc->ram_list.freed;
        context->last_block = uc->ram_list.last_block;
        context->snapshot_level = uc->snapshot_level;
        uc->tcg_flush_tlb(uc);
    }

    if (uc->context_content & UC_CTL_CONTEXT_CPU) {
        if (!uc->context_save) {
            memcpy(context->data, uc->cpu->env_ptr, data_size);
            restore_jit_state(uc);
            return UC_ERR_OK;
        } else {
            ret = uc->context_save(uc, context);
            restore_jit_state(uc);
            return ret;
        }
    }
    restore_jit_state(uc);
    return ret;
}

// Keep in mind that we don't a uc_engine when r/w the registers of a context.
static context_reg_rw_t find_context_reg_rw(uc_arch arch, uc_mode mode)
{
    // We believe that the arch/mode pair is correct.
    context_reg_rw_t rw = {default_reg_read, default_reg_write};
    switch (arch) {
    default:
        break;
#ifdef UNICORN_HAS_M68K
    case UC_ARCH_M68K:
        rw.read = reg_read_m68k;
        rw.write = reg_write_m68k;
        break;
#endif
#ifdef UNICORN_HAS_X86
    case UC_ARCH_X86:
        rw.read = reg_read_x86_64;
        rw.write = reg_write_x86_64;
        break;
#endif
#ifdef UNICORN_HAS_ARM
    case UC_ARCH_ARM:
        rw.read = reg_read_arm;
        rw.write = reg_write_arm;
        break;
#endif
#ifdef UNICORN_HAS_ARM64
    case UC_ARCH_ARM64:
        rw.read = reg_read_aarch64;
        rw.write = reg_write_aarch64;
        break;
#endif

#if defined(UNICORN_HAS_MIPS) || defined(UNICORN_HAS_MIPSEL) ||                \
    defined(UNICORN_HAS_MIPS64) || defined(UNICORN_HAS_MIPS64EL)
    case UC_ARCH_MIPS:
        if (mode & UC_MODE_BIG_ENDIAN) {
#ifdef UNICORN_HAS_MIPS
            if (mode & UC_MODE_MIPS32) {
                rw.read = reg_read_mips;
                rw.write = reg_write_mips;
            }
#endif
#ifdef UNICORN_HAS_MIPS64
            if (mode & UC_MODE_MIPS64) {
                rw.read = reg_read_mips64;
                rw.write = reg_write_mips64;
            }
#endif
        } else { // little endian
#ifdef UNICORN_HAS_MIPSEL
            if (mode & UC_MODE_MIPS32) {
                rw.read = reg_read_mipsel;
                rw.write = reg_write_mipsel;
            }
#endif
#ifdef UNICORN_HAS_MIPS64EL
            if (mode & UC_MODE_MIPS64) {
                rw.read = reg_read_mips64el;
                rw.write = reg_write_mips64el;
            }
#endif
        }
        break;
#endif

#ifdef UNICORN_HAS_SPARC
    case UC_ARCH_SPARC:
        if (mode & UC_MODE_SPARC64) {
            rw.read = reg_read_sparc64;
            rw.write = reg_write_sparc64;
        } else {
            rw.read = reg_read_sparc;
            rw.write = reg_write_sparc;
        }
        break;
#endif
#ifdef UNICORN_HAS_PPC
    case UC_ARCH_PPC:
        if (mode & UC_MODE_PPC64) {
            rw.read = reg_read_ppc64;
            rw.write = reg_write_ppc64;
        } else {
            rw.read = reg_read_ppc;
            rw.write = reg_write_ppc;
        }
        break;
#endif
#ifdef UNICORN_HAS_RISCV
    case UC_ARCH_RISCV:
        if (mode & UC_MODE_RISCV32) {
            rw.read = reg_read_riscv32;
            rw.write = reg_write_riscv32;
        } else if (mode & UC_MODE_RISCV64) {
            rw.read = reg_read_riscv64;
            rw.write = reg_write_riscv64;
        }
        break;
#endif
#ifdef UNICORN_HAS_S390X
    case UC_ARCH_S390X:
        rw.read = reg_read_s390x;
        rw.write = reg_write_s390x;
        break;
#endif
#ifdef UNICORN_HAS_TRICORE
    case UC_ARCH_TRICORE:
        rw.read = reg_read_tricore;
        rw.write = reg_write_tricore;
        break;
#endif
    }

    return rw;
}

UNICORN_EXPORT
uc_err uc_context_reg_write(uc_context *ctx, int regid, const void *value)
{
    int setpc = 0;
    size_t size = (size_t)-1;
    return find_context_reg_rw(ctx->arch, ctx->mode)
        .write(ctx->data, ctx->mode, regid, value, &size, &setpc);
}

UNICORN_EXPORT
uc_err uc_context_reg_read(uc_context *ctx, int regid, void *value)
{
    size_t size = (size_t)-1;
    return find_context_reg_rw(ctx->arch, ctx->mode)
        .read(ctx->data, ctx->mode, regid, value, &size);
}

UNICORN_EXPORT
uc_err uc_context_reg_write2(uc_context *ctx, int regid, const void *value,
                             size_t *size)
{
    int setpc = 0;
    return find_context_reg_rw(ctx->arch, ctx->mode)
        .write(ctx->data, ctx->mode, regid, value, size, &setpc);
}

UNICORN_EXPORT
uc_err uc_context_reg_read2(uc_context *ctx, int regid, void *value,
                            size_t *size)
{
    return find_context_reg_rw(ctx->arch, ctx->mode)
        .read(ctx->data, ctx->mode, regid, value, size);
}

UNICORN_EXPORT
uc_err uc_context_reg_write_batch(uc_context *ctx, int const *regs,
                                  void *const *vals, int count)
{
    reg_write_t reg_write = find_context_reg_rw(ctx->arch, ctx->mode).write;
    void *env = ctx->data;
    int mode = ctx->mode;
    int setpc = 0;
    int i;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        const void *value = vals[i];
        size_t size = (size_t)-1;
        uc_err err = reg_write(env, mode, regid, value, &size, &setpc);
        if (err) {
            return err;
        }
    }

    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_context_reg_read_batch(uc_context *ctx, int const *regs, void **vals,
                                 int count)
{
    reg_read_t reg_read = find_context_reg_rw(ctx->arch, ctx->mode).read;
    void *env = ctx->data;
    int mode = ctx->mode;
    int i;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        void *value = vals[i];
        size_t size = (size_t)-1;
        uc_err err = reg_read(env, mode, regid, value, &size);
        if (err) {
            return err;
        }
    }

    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_context_reg_write_batch2(uc_context *ctx, int const *regs,
                                   const void *const *vals, size_t *sizes,
                                   int count)
{
    reg_write_t reg_write = find_context_reg_rw(ctx->arch, ctx->mode).write;
    void *env = ctx->data;
    int mode = ctx->mode;
    int setpc = 0;
    int i;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        const void *value = vals[i];
        uc_err err = reg_write(env, mode, regid, value, sizes + i, &setpc);
        if (err) {
            return err;
        }
    }

    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_context_reg_read_batch2(uc_context *ctx, int const *regs,
                                  void *const *vals, size_t *sizes, int count)
{
    reg_read_t reg_read = find_context_reg_rw(ctx->arch, ctx->mode).read;
    void *env = ctx->data;
    int mode = ctx->mode;
    int i;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        void *value = vals[i];
        uc_err err = reg_read(env, mode, regid, value, sizes + i);
        if (err) {
            return err;
        }
    }

    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_context_restore(uc_engine *uc, uc_context *context)
{
    UC_INIT(uc);
    uc_err ret;
    size_t capacity = 0;

    if (!context_matches_arch_mode(uc, context)) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }
    if (context_allocation_capacity(context, &capacity) &&
        context->context_size > capacity) {
        restore_jit_state(uc);
        return UC_ERR_ARG;
    }

    if (uc->context_content & UC_CTL_CONTEXT_CPU) {
        if (uc->context_validate) {
            ret = uc->context_validate(uc, context);
            if (ret != UC_ERR_OK) {
                restore_jit_state(uc);
                return ret;
            }
        } else if (context->context_size < context_data_size(uc)) {
            restore_jit_state(uc);
            return UC_ERR_ARG;
        }
    }

    if (uc->context_content & UC_CTL_CONTEXT_MEMORY) {
        uint64_t active_tb_mask;
        FlatView *restore_view;
        uint32_t i;
        bool copied;

        if (!context->fv || context->memory_owner != uc) {
            restore_jit_state(uc);
            return UC_ERR_ARG;
        }
        if (test_alloc_should_fail(uc, UC_TEST_ALLOC_FAIL_RESTORE_VIEW)) {
            restore_view = NULL;
        } else {
            restore_view = g_try_malloc0(sizeof(*restore_view));
        }
        if (!restore_view) {
            restore_jit_state(uc);
            return UC_ERR_NOMEM;
        }
        restore_view->ref = 1;
        ret = uc_restore_snapshot_preflight(uc, context, restore_view);
        if (ret != UC_ERR_OK) {
            g_free(restore_view->ranges);
            g_free(restore_view);
            restore_jit_state(uc);
            return ret;
        }

        active_tb_mask = all_active_tb_mask(uc);
        mark_active_tbs_for_exit(uc, active_tb_mask);
        request_tb_flush(uc);

        while (uc->mapped_block_count != 0) {
            UcMapping *mapping = uc->mapped_blocks[uc->mapped_block_count - 1];

            uc->memory_moveout(uc, mapping, false);
            mem_map_remove(uc, mapping);
        }

        for (i = 0; i < context->mapping_count; i++) {
            UcContextMapping *saved = &context->mappings[i];
            UcMapping *mapping = saved->mapping;

            uc->memory_restore_topology(
                uc, mapping, &context->memory_regions[saved->first_region],
                saved->region_count, false);
            uc->memory_movein(uc, mapping, false);
            mem_map_activate(uc, mapping);
        }

        copied = uc->flatview_copy(uc, restore_view, context->fv, false);
        assert(copied);
        (void)copied;
        uc->address_space_restore_flatview(&uc->address_space_memory,
                                           restore_view);
        mapping_reclaim_inactive(uc);

        if (uc->nested_level != 0) {
            uc->quit_request = true;
            break_translation_loop(uc);
        }
        uc->tcg_flush_tlb(uc);
    }

    if (uc->context_content & UC_CTL_CONTEXT_CPU) {
        if (!uc->context_restore) {
            size_t data_size = context_data_size(uc);

            memcpy(uc->cpu->env_ptr, context->data, data_size);
            if (uc->nested_level != 0) {
                uc_request_pc_change(uc);
            }
            restore_jit_state(uc);
            return UC_ERR_OK;
        } else {
            ret = uc->context_restore(uc, context);
            if (ret == UC_ERR_OK && uc->nested_level != 0) {
                uc_request_pc_change(uc);
            }
            restore_jit_state(uc);
            return ret;
        }
    }
    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_context_free(uc_context *context)
{
    UcContextAllocation *allocation = NULL;
    UcContextAllocation **link;

    if (!context) {
        return UC_ERR_ARG;
    }
    while (emu_atomic_cmpxchg(&context_allocations_lock, 0, 1) != 0) {
    }
    for (link = &context_allocations; *link; link = &(*link)->next) {
        if ((*link)->context == context) {
            allocation = *link;
            *link = allocation->next;
            break;
        }
    }
    emu_atomic_set(&context_allocations_lock, 0);
    if (!allocation) {
        return UC_ERR_ARG;
    }

    context_memory_clear(context, true, true);
    return uc_free(allocation);
}

typedef struct _uc_ctl_exit_request {
    uint64_t *array;
    size_t len;
} uc_ctl_exit_request;

static inline gboolean uc_read_exit_iter(gpointer key, gpointer val,
                                         gpointer data)
{
    uc_ctl_exit_request *req = (uc_ctl_exit_request *)data;

    req->array[req->len++] = *(uint64_t *)key;

    return false;
}

UNICORN_EXPORT
uc_err uc_ctl(uc_engine *uc, uc_control_type control, ...)
{
    int rw, type;
    uc_err err = UC_ERR_OK;
    va_list args;

    // MSVC Would do signed shift on signed integers.
    rw = (uint32_t)control >> 30;
    type = (control & ((1 << 16) - 1));
    va_start(args, control);

    switch (type) {
    case UC_CTL_UC_MODE: {
        if (rw == UC_CTL_IO_READ) {
            int *pmode = va_arg(args, int *);
            *pmode = uc->mode;
        } else {
            err = UC_ERR_ARG;
        }
        break;
    }

    case UC_CTL_UC_ARCH: {
        if (rw == UC_CTL_IO_READ) {
            int *arch = va_arg(args, int *);
            *arch = uc->arch;
        } else {
            err = UC_ERR_ARG;
        }
        break;
    }

    case UC_CTL_UC_TIMEOUT: {
        if (rw == UC_CTL_IO_READ) {
            uint64_t *arch = va_arg(args, uint64_t *);
            *arch = uc->timeout;
        } else {
            err = UC_ERR_ARG;
        }
        break;
    }

    case UC_CTL_UC_PAGE_SIZE: {
        if (rw == UC_CTL_IO_READ) {

            UC_INIT(uc);

            uint32_t *page_size = va_arg(args, uint32_t *);
            *page_size = uc->target_page_size;

            restore_jit_state(uc);
        } else {
            uint32_t page_size = va_arg(args, uint32_t);
            int bits = 0;

            if (uc->init_done) {
                err = UC_ERR_ARG;
                break;
            }

            if (uc->arch != UC_ARCH_ARM && uc->arch != UC_ARCH_ARM64) {
                err = UC_ERR_ARG;
                break;
            }

            if (page_size == 0 || (page_size & (page_size - 1))) {
                err = UC_ERR_ARG;
                break;
            }

            // Bits is used to calculate the mask
            while (page_size > 1) {
                bits++;
                page_size >>= 1;
            }

            uc->target_bits = bits;

            err = UC_ERR_OK;
        }

        break;
    }

    case UC_CTL_UC_USE_EXITS: {
        if (rw == UC_CTL_IO_WRITE) {
            int use_exits = va_arg(args, int);

            uc->use_exits = use_exits;
            if (!use_exits && uc->ctl_exits != NULL) {
                g_tree_remove_all(uc->ctl_exits);
            }
        } else {
            err = UC_ERR_ARG;
        }
        break;
    }

    case UC_CTL_UC_EXITS_CNT: {

        UC_INIT(uc);

        if (!uc->use_exits) {
            err = UC_ERR_ARG;
        } else if (rw == UC_CTL_IO_READ) {
            size_t *exits_cnt = va_arg(args, size_t *);
            *exits_cnt = g_tree_nnodes(uc->ctl_exits);
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;
    }

    case UC_CTL_UC_EXITS: {

        UC_INIT(uc);

        if (!uc->use_exits) {
            err = UC_ERR_ARG;
        } else if (rw == UC_CTL_IO_READ) {
            uint64_t *exits = va_arg(args, uint64_t *);
            size_t cnt = va_arg(args, size_t);
            if (cnt < g_tree_nnodes(uc->ctl_exits)) {
                err = UC_ERR_ARG;
            } else {
                uc_ctl_exit_request req;
                req.array = exits;
                req.len = 0;

                g_tree_foreach(uc->ctl_exits, uc_read_exit_iter, (void *)&req);
            }
        } else if (rw == UC_CTL_IO_WRITE) {
            uint64_t *exits = va_arg(args, uint64_t *);
            size_t cnt = va_arg(args, size_t);

            g_tree_remove_all(uc->ctl_exits);

            for (size_t i = 0; i < cnt; i++) {
                uc_add_exit(uc, exits[i]);
            }
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;
    }

    case UC_CTL_CPU_MODEL: {
        if (rw == UC_CTL_IO_READ) {

            UC_INIT(uc);

            int *model = va_arg(args, int *);
            *model = uc->cpu_model;

            restore_jit_state(uc);
        } else {
            int model = va_arg(args, int);

            if (model < 0 || uc->init_done) {
                err = UC_ERR_ARG;
                break;
            }

            if (uc->arch == UC_ARCH_X86) {
                if (model >= UC_CPU_X86_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }
            } else if (uc->arch == UC_ARCH_ARM) {
                if (model >= UC_CPU_ARM_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }

                if (uc->mode & UC_MODE_BIG_ENDIAN) {
                    // These cpu models don't support big endian code access.
                    if (model <= UC_CPU_ARM_CORTEX_A15 &&
                        model >= UC_CPU_ARM_CORTEX_A7) {
                        err = UC_ERR_ARG;
                        break;
                    }
                }
            } else if (uc->arch == UC_ARCH_ARM64) {
                if (model >= UC_CPU_ARM64_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }
            } else if (uc->arch == UC_ARCH_MIPS) {
                if (uc->mode & UC_MODE_32 && model >= UC_CPU_MIPS32_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }

                if (uc->mode & UC_MODE_64 && model >= UC_CPU_MIPS64_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }
            } else if (uc->arch == UC_ARCH_PPC) {
                // UC_MODE_PPC32 == UC_MODE_32
                if (uc->mode & UC_MODE_32 && model >= UC_CPU_PPC32_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }

                if (uc->mode & UC_MODE_64 && model >= UC_CPU_PPC64_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }
            } else if (uc->arch == UC_ARCH_RISCV) {
                if (uc->mode & UC_MODE_32 && model >= UC_CPU_RISCV32_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }

                if (uc->mode & UC_MODE_64 && model >= UC_CPU_RISCV64_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }
            } else if (uc->arch == UC_ARCH_S390X) {
                if (model >= UC_CPU_S390X_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }
            } else if (uc->arch == UC_ARCH_SPARC) {
                if (uc->mode & UC_MODE_32 && model >= UC_CPU_SPARC32_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }
                if (uc->mode & UC_MODE_64 && model >= UC_CPU_SPARC64_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }
            } else if (uc->arch == UC_ARCH_M68K) {
                if (model >= UC_CPU_M68K_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }
            } else if (uc->arch == UC_ARCH_TRICORE) {
                if (model >= UC_CPU_TRICORE_ENDING) {
                    err = UC_ERR_ARG;
                    break;
                }
            } else {
                err = UC_ERR_ARG;
                break;
            }

            uc->cpu_model = model;

            err = UC_ERR_OK;
        }
        break;
    }

    case UC_CTL_TB_REQUEST_CACHE: {

        UC_INIT(uc);

        if (rw == UC_CTL_IO_READ_WRITE) {
            uint64_t addr = va_arg(args, uint64_t);
            uc_tb *tb = va_arg(args, uc_tb *);
            err = uc->uc_gen_tb(uc, addr, tb);
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;
    }

    case UC_CTL_TB_REMOVE_CACHE: {

        UC_INIT(uc);

        if (rw == UC_CTL_IO_WRITE) {
            uint64_t addr = va_arg(args, uint64_t);
            uint64_t end = va_arg(args, uint64_t);
            if (end <= addr) {
                err = UC_ERR_ARG;
            } else {
                uc->uc_invalidate_tb(uc, addr, end - addr);
            }
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;
    }

    case UC_CTL_TB_FLUSH:

        UC_INIT(uc);

        if (rw == UC_CTL_IO_WRITE) {
            request_tb_flush(uc);
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;

    case UC_CTL_TLB_FLUSH:

        UC_INIT(uc);

        if (rw == UC_CTL_IO_WRITE) {
            uc->tcg_flush_tlb(uc);
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;

    case UC_CTL_TLB_TYPE: {

        UC_INIT(uc);

        if (rw == UC_CTL_IO_WRITE) {
            int mode = va_arg(args, int);
            err = uc->set_tlb(uc, mode);
            if (err == UC_ERR_OK) {
                uc->tcg_flush_tlb(uc);
            }
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;
    }

    case UC_CTL_TCG_BUFFER_SIZE: {
        if (rw == UC_CTL_IO_WRITE) {
            uint32_t size = va_arg(args, uint32_t);
            uc->tcg_buffer_size = size;
        } else {

            UC_INIT(uc);

            uint32_t *size = va_arg(args, uint32_t *);
            *size = uc->tcg_buffer_size;

            restore_jit_state(uc);
        }
        break;
    }

    case UC_CTL_CONTEXT_MODE:

        UC_INIT(uc);

        if (rw == UC_CTL_IO_WRITE) {
            int mode = va_arg(args, int);
            uc->context_content = mode;
            err = UC_ERR_OK;
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;

    case UC_CTL_PAUTH_SIGN: {

        UC_INIT(uc);

        if (rw == UC_CTL_IO_READ_WRITE) {
            uint64_t ptr = va_arg(args, uint64_t);
            int key = va_arg(args, int);
            uint64_t diversifier = va_arg(args, uint64_t);
            uint64_t *signed_ptr = va_arg(args, uint64_t *);
            if (uc->pauth_sign != NULL) {
                err = uc->pauth_sign(uc, ptr, key, diversifier, signed_ptr);
            } else {
                err = UC_ERR_ARG;
            }
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;
    }

    case UC_CTL_PAUTH_STRIP: {

        UC_INIT(uc);

        if (rw == UC_CTL_IO_READ_WRITE) {
            uint64_t ptr = va_arg(args, uint64_t);
            int key = va_arg(args, int);
            uint64_t *stripped_ptr = va_arg(args, uint64_t *);
            if (uc->pauth_strip != NULL) {
                err = uc->pauth_strip(uc, ptr, key, stripped_ptr);
            } else {
                err = UC_ERR_ARG;
            }
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;
    }

    case UC_CTL_PAUTH_AUTH: {

        UC_INIT(uc);

        if (rw == UC_CTL_IO_READ_WRITE) {
            uint64_t ptr = va_arg(args, uint64_t);
            int key = va_arg(args, int);
            uint64_t diversifier = va_arg(args, uint64_t);
            bool *valid = va_arg(args, bool *);
            if (uc->pauth_auth != NULL) {
                err = uc->pauth_auth(uc, ptr, key, diversifier, valid);
            } else {
                err = UC_ERR_ARG;
            }
        } else {
            err = UC_ERR_ARG;
        }

        restore_jit_state(uc);
        break;
    }
    case UC_CTL_INVALID_ADDR:
        if (rw == UC_CTL_IO_READ) {
            uint64_t *invalid_addr = va_arg(args, uint64_t *);
            *invalid_addr = uc->invalid_addr;
        } else {
            err = UC_ERR_ARG;
        }
        break;

    case UC_CTL_UC_PREALLOC: {
        if (rw == UC_CTL_IO_WRITE) {
#if defined(WIN32) && defined(WIN32_ENABLE_VEH)
            int prealloc = va_arg(args, int);

            // Must be decided before the code gen buffer is allocated.
            if (uc->init_done) {
                err = UC_ERR_ARG;
            } else {
                uc->prealloc = (bool)prealloc;
            }
#elif defined(WIN32)
            // The VEH lazy-commit path is compiled out, so preallocation is
            // mandatory on Windows. Refuse to disable it; enabling it is
            // already the effective behavior.
            int prealloc = va_arg(args, int);

            if (uc->init_done || !prealloc) {
                err = UC_ERR_ARG;
            }
#else
            // Not applicable on Linux/macOS: the code gen buffer is always
            // committed and there is no VEH to opt out of.
            err = UC_ERR_ARG;
#endif
        } else {
            err = UC_ERR_ARG;
        }
        break;
    }

    default:
        err = UC_ERR_ARG;
        break;
    }

    va_end(args);

    return err;
}

static uc_err uc_snapshot(struct uc_struct *uc)
{
    if (uc->snapshot_level == INT32_MAX) {
        return UC_ERR_RESOURCE;
    }
    uc->snapshot_level++;
    return UC_ERR_OK;
}

static uc_err uc_restore_snapshot_preflight(uc_engine *uc,
                                            const uc_context *context,
                                            FlatView *restore_view)
{
    uint32_t consumed_regions = 0;
    UcMapping *previous = NULL;
    uint32_t i;

    if (context->memory_owner != uc || context->snapshot_level <= 0 ||
        (context->mapping_count != 0 && !context->mappings) ||
        (context->memory_region_count != 0 && !context->memory_regions) ||
        (context->fv->nr != 0 && !context->fv->ranges)) {
        return UC_ERR_ARG;
    }

    for (i = 0; i < context->mapping_count; i++) {
        const UcContextMapping *saved = &context->mappings[i];
        UcMapping *mapping = saved->mapping;
        uint32_t j;

        if (!mapping || mapping->owner != uc || !mapping->root ||
            mapping->size == 0 ||
            mapping->begin > UINT64_MAX - (mapping->size - 1) ||
            saved->region_count == 0 ||
            saved->first_region != consumed_regions ||
            saved->region_count >
                context->memory_region_count - consumed_regions) {
            return UC_ERR_ARG;
        }
        if (previous &&
            (mapping->begin < previous->begin ||
             ranges_overlap(previous->begin, previous->size, mapping->begin,
                            mapping->size))) {
            return UC_ERR_MAP;
        }
        previous = mapping;

        if (mapping->root->terminates &&
            (saved->region_count != 1 ||
             context->memory_regions[saved->first_region] != mapping->root)) {
            return UC_ERR_ARG;
        }

        for (j = 0; j < saved->region_count; j++) {
            MemoryRegion *region =
                context->memory_regions[saved->first_region + j];

            if (!region || region->uc_mapping != mapping ||
                (!mapping->root->terminates && region == mapping->root)) {
                return UC_ERR_ARG;
            }
        }
        consumed_regions += saved->region_count;
    }

    if (consumed_regions != context->memory_region_count) {
        return UC_ERR_ARG;
    }
    if (!mem_map_reserve(uc, context->mapping_count) ||
        !uc->flatview_reserve(restore_view, context->fv->nr)) {
        return UC_ERR_NOMEM;
    }
    return UC_ERR_OK;
}

#ifdef UNICORN_TRACER
uc_tracer *get_tracer()
{
    static uc_tracer tracer;
    return &tracer;
}

void trace_start(uc_tracer *tracer, trace_loc loc)
{
    tracer->starts[loc] = get_clock();
}

void trace_end(uc_tracer *tracer, trace_loc loc, const char *fmt, ...)
{
    va_list args;
    int64_t end = get_clock();

    va_start(args, fmt);

    vfprintf(stderr, fmt, args);

    va_end(args);

    fprintf(stderr, "%.6fus\n",
            (double)(end - tracer->starts[loc]) / (double)(1000));
}
#endif
