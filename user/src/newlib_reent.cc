// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Where libc finds the running thread's struct _reent, the reader of the running thread's lock
// id, and the exit-time release of the scratch libc allocates into a thread's state. The user
// side of kernel/include/kickos/reent.h; compiled on every board but the sim, whose libc is the
// host's.
//
// Each thread's state is the base of its own stack block, and the cell that ends the carve
// names it. Newlib reaches the state through __getreent() on every target: the running thread's
// cell on lx6, armv8a, rv64imac and x86_64, and the _impure_ptr the kernel seats at each switch
// on the single-core Cortex-M, RV32 and RX targets. Every cross target links the KickOS
// toolchain's own newlib; errno is a member of that state.

#include <kickos/config/system.h> // KICKOS_STACK_ALIGN
#include <kickos/reent.h>
#include <kickos/sys.h>

#include <malloc.h>
#include <sys/reent.h>

#if KICKOS_REENT_PER_THREAD and not defined(__x86_64__)
// This image's offset in the link's one thread-local segment: the thread pointer sits that far
// below the cell (<kickos/sys/image.h>).
extern "C" char const __kickos_image_tls_offset[];
#endif

namespace
{
    // The carve's last words, ending where the thread's TLS block starts. The kernel writes
    // them at thread creation, the runtime's words zero.
    struct ReentCell
    {
        KickosThreadWords words;
        struct _reent* state;
        uintptr_t self;
    };
    static_assert(sizeof(ReentCell) == KICKOS_REENT_CELL_WORDS * sizeof(void*),
                  "the cell is the KICKOS_REENT_CELL_WORDS words the kernel seats");

    static_assert(KICKOS_REENT_CARVE
                      == ((sizeof(struct _reent) + sizeof(ReentCell) + (KICKOS_STACK_ALIGN - 1u))
                          & ~static_cast<size_t>(KICKOS_STACK_ALIGN - 1u)),
                  "KICKOS_REENT_CARVE in the root CMakeLists.txt is not this newlib's struct "
                  "_reent plus the cell, rounded to the stack alignment");

#if not defined(__x86_64__)
    ReentCell const* running_cell()
    {
#if KICKOS_REENT_PER_THREAD
        unsigned char const* const tp
            = static_cast<unsigned char const*>(__builtin_thread_pointer());
        return reinterpret_cast<ReentCell const*>(
                   tp + reinterpret_cast<uintptr_t>(__kickos_image_tls_offset))
               - 1;
#else
        unsigned char const* const state = reinterpret_cast<unsigned char const*>(_impure_ptr);
        return reinterpret_cast<ReentCell const*>(state + KICKOS_REENT_CARVE) - 1;
#endif
    }
#endif
}

#define KOS_REENT_STR2(x) #x
#define KOS_REENT_STR(x) KOS_REENT_STR2(x)
#define KOS_REENT_SYM(name) KOS_REENT_STR(__USER_LABEL_PREFIX__) #name

// What the image header carries of this profile, as absolute symbols: each image's header takes
// its user library's, so the header compiles the same for either profile.
asm(".globl " KOS_REENT_SYM(__kickos_reent_width) "\n\t"
    ".set " KOS_REENT_SYM(__kickos_reent_width) ", %c0\n\t"
    ".globl " KOS_REENT_SYM(__kickos_reent_carve) "\n\t"
    ".set " KOS_REENT_SYM(__kickos_reent_carve) ", %c1"
    :
    : "i"(sizeof(struct _reent)), "i"(KICKOS_REENT_CARVE));

// %fs:0 holds no self pointer, so x86_64 reads the cell at its negative offsets from the FS base.
#if defined(__x86_64__)
extern "C" struct _reent* __getreent(void)
{
    struct _reent* state;
    __asm__("movq %%fs:-16, %0" : "=r"(state));
    return state;
}

extern "C" uint32_t kickos_self_lock_id(void)
{
    uintptr_t self;
    __asm__("movq %%fs:-8, %0" : "=r"(self));
    return static_cast<uint32_t>(self);
}
#else
// Every libc that asks __getreent ships none of its own (esp-elf's is a weak NULL), so this is
// what makes stdio work at all.
#if KICKOS_REENT_PER_THREAD
extern "C" struct _reent* __getreent(void)
{
    return running_cell()->state;
}
#else
extern "C" struct _reent* __getreent(void)
{
    return _impure_ptr;
}
#endif

extern "C" uint32_t kickos_self_lock_id(void)
{
    return static_cast<uint32_t>(running_cell()->self);
}
#endif

// The running thread's cell ends its carve, whose base is its state on every target.
extern "C" KickosThreadWords* kickos_thread_words(void)
{
    unsigned char* const state = reinterpret_cast<unsigned char*>(__getreent());
    return &(reinterpret_cast<ReentCell*>(state + KICKOS_REENT_CARVE) - 1)->words;
}

// The threads layer (gthr.cc), which an image that creates a key links, defines the real one.
extern "C" __attribute__((weak)) void kickos_keys_thread_end(void)
{
}

// thread_atexit.cc, which an image with a thread_local destructor links, defines the real one.
extern "C" __attribute__((weak)) void kickos_thread_dtors_run(void)
{
}

namespace
{
    // Balloc sizes the freelist _Kmax + 1, one more than _reclaim_reent walks.
    void release_mprec(struct _reent* r)
    {
        struct _Bigint** const freelist = _REENT_MP_FREELIST(r);
        if (freelist != nullptr)
        {
            for (size_t k = 0; k <= _Kmax; k++)
            {
                struct _Bigint* b = freelist[k];
                while (b != nullptr)
                {
                    struct _Bigint* const next = b->_next;
                    _free_r(r, b);
                    b = next;
                }
            }
            _free_r(r, freelist);
            _REENT_MP_FREELIST(r) = nullptr;
        }
        _free_r(r, _REENT_MP_RESULT(r));
        _REENT_MP_RESULT(r) = nullptr;
        struct _Bigint* p5 = _REENT_MP_P5S(r);
        while (p5 != nullptr)
        {
            struct _Bigint* const next = p5->_next;
            _free_r(r, p5);
            p5 = next;
        }
        _REENT_MP_P5S(r) = nullptr;
    }

    // Not _reclaim_reent: it runs the stdio cleanup hook, which closes the FILEs every thread
    // shares. The signal table goes too, unlike there: no signal reaches a thread that is exiting.
    void release_scratch(void)
    {
        struct _reent* const r = _REENT;
#ifdef _REENT_SMALL
        if (r->_mp != nullptr)
        {
            release_mprec(r);
            _free_r(r, r->_mp);
            r->_mp = nullptr;
        }
        _free_r(r, r->_emergency);
        r->_emergency = nullptr;
        _free_r(r, r->_r48);
        r->_r48 = nullptr;
        _free_r(r, r->_localtime_buf);
        r->_localtime_buf = nullptr;
        _free_r(r, r->_asctime_buf);
        r->_asctime_buf = nullptr;
        _free_r(r, r->_misc);
        r->_misc = nullptr;
        _free_r(r, r->_signal_buf);
        r->_signal_buf = nullptr;
#else
        release_mprec(r);
#endif
        _free_r(r, _REENT_CVTBUF(r));
        _REENT_CVTBUF(r) = nullptr;
        _REENT_CVTLEN(r) = 0;
        _free_r(r, _REENT_SIG_FUNC(r));
        _REENT_SIG_FUNC(r) = nullptr;
    }

    // Never called directly: a direct call would keep the allocator in every image.
    void (*s_release)(void) = nullptr;
}

extern "C"
{

void kickos_reent_heap_used(void)
{
    __atomic_store_n(&s_release, &release_scratch, __ATOMIC_RELAXED);
}

// Relaxed: a thread that built scratch took the malloc lock after the heap seat's store.
void kickos_reent_release(void)
{
    void (*const release)(void) = __atomic_load_n(&s_release, __ATOMIC_RELAXED);
    if (release != nullptr)
    {
        release();
    }
}
}
