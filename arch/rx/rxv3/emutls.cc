// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// __emutls_get_address for rxv3, overriding libgcc's. GNURX has no native TLS, so the
// compiler lowers every thread_local access to a call to this leaf. The
// libgcc.a(emutls.o) that would otherwise answer it is the SINGLE-THREADED build: it
// caches one pointer in the control block and hands every thread the same object, with no
// diagnostic anywhere. Defining the symbol here keeps that member unextracted, which is
// the archive rule arch/CMakeLists.txt states, and drops emutls.o's malloc/sbrk chain with
// it (sbrk references `end`, which no KickOS linker script defines).

#include <stddef.h>
#include <stdint.h>

#if defined(KICKOS_TLS) && KICKOS_TLS

// Bytes carved off the bottom of every thread's stack for this image's thread_local
// storage. A KNOB, where the five backends with real TLS relocations have a MEASUREMENT:
// there the linker lays .tdata and .tbss out and the carve is their size, so the fit is
// proven at link time. GNURX emits neither section; each object carries its own size in a
// word the linker never sums, so on RX the fit is a RUNTIME check and rx_tls_panic() below
// is what it costs when it fails. A multiple of 16 so it survives tls_block_size()'s
// KICKOS_STACK_ALIGN rounding unchanged.
#ifndef KICKOS_RX_TLS_BLOCK
#define KICKOS_RX_TLS_BLOCK 256
#endif

// THE RESERVATION THE KERNEL'S CARVE IS SIZED FROM. kernel/thread/tls.cc measures the
// carve as .tdata + .tbss; GNURX emits neither, so without this every thread's block is
// zero bytes and thread_create skips the carve, leaving nothing between the bump allocator
// below and live stack. It sits in THIS translation unit because the member is extracted
// only when something references __emutls_get_address, so an image with no thread_local
// reserves nothing, exactly as on the other five backends.
__attribute__((used, section(".tbss"), aligned(8)))
static unsigned char g_tls_reserve[KICKOS_RX_TLS_BLOCK];

extern "C"
{
    // The bounds of the image's emutls control blocks, gathered contiguously by the chip
    // linker script. RX psABI: C's __kickos_* is asm ___kickos_*.
    extern unsigned char __kickos_emutls_v_start[];
    extern unsigned char __kickos_emutls_v_end[];
    // The reservation above, as the kernel measures it (kernel/thread/tls.cc).
    extern unsigned char __kickos_tbss_start[];
    extern unsigned char __kickos_tbss_end[];
    // Code flash, the only region a control block's template may point into. The CODE
    // pair and not the ROM pair: __kickos_rom_end is ORIGIN(FVECT) + LENGTH(FVECT), which
    // is 0x1_0000_0000 and WRAPS TO 0 in a 32-bit uintptr_t, so an upper bound written
    // against it admits every address above the base. __kickos_code_end stops at
    // 0xFFFFFF80, the start of the fixed vectors, which no template is in anyway.
    extern unsigned char __kickos_code_start[];
    extern unsigned char __kickos_code_end[];
}

namespace
{
    // What GCC emits per thread_local object, 16 bytes in .data (probed on GNURX 14.2:
    // .data.__emutls_v.<name>, size 0x10, align 4).
    //
    // `loc` IS UPSTREAM'S PROCESS-WIDE POINTER CACHE AND THIS FILE NEVER TOUCHES IT.
    // The per-thread index is the object's own position in the gathered array instead, so
    // there is no first-use assignment for two threads to race over and no writable global
    // outside the calling thread's own block. That matters twice on RX: this leaf runs
    // unprivileged, and libkickos_arch_rxv3.a's .data lands on the KERNEL side of the
    // enforcement split, where an unprivileged store faults.
    struct EmutlsObject
    {
        uint32_t size;
        uint32_t align;
        uint32_t loc;
        unsigned char const* templ;
    };
    static_assert(sizeof(EmutlsObject) == 16,
                  "the gathered __emutls_v array is indexed on a 16-byte stride");

    // Block layout at the thread pointer: a 4-byte header, then one uint16_t offset per
    // emutls object in the image, then the bump area. Every offset is a byte offset from
    // the thread pointer, and 0 means "not allocated in this thread yet"; the header can
    // never live at a non-zero offset, so 0 is free as a sentinel.
    constexpr size_t HEADER_BYTES = 4;
    constexpr size_t PAYLOAD_ALIGN = 8;
    constexpr size_t BLOCK_BYTES = KICKOS_RX_TLS_BLOCK;

    size_t align_up(size_t v, size_t a)
    {
        return (v + (a - 1u)) & ~(a - 1u);
    }

    // BRK, reachable from unprivileged code with no kernel read. The relocatable vector's
    // slot 0 reaches _rx_trap (chip startup.S), which reports cause 0, and cause 0 is
    // deliberately outside rx_cause_is_thread_fault()'s set, so this stops the system
    // rather than killing one thread.
    //
    // A SYSTEM PANIC AND NOT A THREAD KILL, FOR THE FORGED ANCHOR TOO. rx72m-flat runs its
    // threads in supervisor, so PSW.PM is never set and arch_fault_is_user_thread refuses
    // every cause there: no encoding kills one thread on all three variants. The one thing
    // that must not happen either way is returning storage some other thread also holds.
    [[noreturn]] void rx_tls_panic()
    {
        // AND THIS IS ALSO WHAT ANCHORS THE RESERVATION. The .tbss blob carries no
        // relocation of its own, and the template in arch/common/sections.ld.h does not
        // KEEP it, so without a reference from a live section --gc-sections drops it and
        // the carve the kernel measures collapses back to zero bytes. It sits on the
        // panic path so the hot path pays nothing for it.
        __asm__ volatile("" : : "r"(&g_tls_reserve[0]));
        while (true)
        {
            __asm__ volatile("brk");
        }
    }
}

// Answers every thread_local access on rxv3, UNPRIVILEGED AND IN THE CALLING THREAD. It
// cannot read the current TCB out of a kernel global: on an enforcing board that load
// faults. RX hands unprivileged code no register that differs per thread except the stack
// pointer, so the thread pointer is derived from R0 and every arena block is strided by
// KICKOS_TLS_STRIDE (arch_ram_region_align).
//
// THE SUBTRACT IS NOT DEFENSIVE, IT IS THE EDGE OF THE RANGE. A stack top is EXCLUSIVE, so
// a thread with an empty stack has R0 exactly at base + stride, which masks to the NEXT
// block and hands it its neighbour's storage. Masking R0 - 1 puts every R0 in
// (base, base + stride] on base. R0 == base cannot occur: the block below is carved off the
// stack and ctx.stack_lo is raised above it.
//
// ON RX THE EDGE IS ALSO UNREACHABLE, WHICH IS NOT A REASON TO DROP THE SUBTRACT. Every
// call site is a bsr, and bsr PUSHES the return address, so R0 here is already at least
// four bytes below the caller's; the exclusive top cannot be observed from inside this
// function the way it is from ARM's __aeabi_read_tp, which a bl reaches with the caller's
// own SP intact. Nothing on this bench witnesses a mutation that removes it, and nothing
// on hardware would either.
extern "C" void* __emutls_get_address(void* anchor)
{
    // THE ANCHOR IS THE ONLY UNTRUSTED INPUT, and everything below walks a table reached
    // through it, so it is checked before it is dereferenced: it must be one of the
    // image's own control blocks, on the 16-byte grid the gather produces.
    uintptr_t const a = reinterpret_cast<uintptr_t>(anchor);
    uintptr_t const vstart = reinterpret_cast<uintptr_t>(__kickos_emutls_v_start);
    uintptr_t const vend = reinterpret_cast<uintptr_t>(__kickos_emutls_v_end);
    if (a < vstart or a >= vend)
    {
        rx_tls_panic();
    }
    size_t const byte_index = static_cast<size_t>(a - vstart);
    if ((byte_index % sizeof(EmutlsObject)) != 0)
    {
        rx_tls_panic();
    }
    size_t const index = byte_index / sizeof(EmutlsObject);

    // The carve the kernel actually made, cross-checked against the reservation this file
    // contributes: a second .tbss contributor would move one and not the other.
    size_t const reserved =
        static_cast<size_t>(__kickos_tbss_end - __kickos_tbss_start);
    if (reserved != BLOCK_BYTES)
    {
        rx_tls_panic();
    }
    size_t const nslots = static_cast<size_t>(vend - vstart) / sizeof(EmutlsObject);
    size_t const data_start = align_up(HEADER_BYTES + 2u * nslots, PAYLOAD_ALIGN);
    if (data_start >= BLOCK_BYTES)
    {
        rx_tls_panic();
    }

    uintptr_t sp;
    __asm__ volatile("mov.l r0, %0" : "=r"(sp));
    unsigned char* const tp = reinterpret_cast<unsigned char*>(
        (sp - 1u) & ~static_cast<uintptr_t>(KICKOS_TLS_STRIDE - 1u));

    // tls_seat() zeroes the whole carve at thread_create and nothing else writes the
    // header, so brk == 0 means this thread has allocated nothing yet. A brk outside the
    // block is a block that was never seated: idle's stack is below one stride so it takes
    // no carve, and masking an SP inside it lands on whatever precedes the arena.
    uint16_t* const brk = reinterpret_cast<uint16_t*>(tp);
    uint16_t* const table = reinterpret_cast<uint16_t*>(tp + HEADER_BYTES);
    if (*brk == 0)
    {
        *brk = static_cast<uint16_t>(data_start);
    }
    else if (*brk < data_start or *brk > BLOCK_BYTES)
    {
        rx_tls_panic();
    }

    // BOUNDED ON THE FAST PATH TOO. The table lives in the thread's own block, which the
    // thread can write, and an offset is only 16 bits: an unchecked one of 65535 lands
    // eight strides away, inside a NEIGHBOUR's block, which is the cross-thread sharing
    // this whole file exists to remove.
    uint16_t const cached = table[index];
    if (cached != 0)
    {
        if (cached < data_start or cached >= BLOCK_BYTES)
        {
            rx_tls_panic();
        }
        return tp + cached;
    }

    EmutlsObject const* const obj = static_cast<EmutlsObject const*>(anchor);
    // Read out of .appdata, which every peer thread in the task can write, so bounded
    // before it is used to move the bump pointer. A forged size that survived would hand
    // this thread a slot overlapping its neighbour's, which is the sharing this file
    // removes.
    size_t const want = obj->size;
    size_t const want_align = obj->align;
    if (want == 0 or want > BLOCK_BYTES)
    {
        rx_tls_panic();
    }
    if (want_align == 0 or want_align > PAYLOAD_ALIGN
        or (want_align & (want_align - 1u)) != 0)
    {
        rx_tls_panic();
    }

    size_t const at = align_up(*brk, want_align);
    if (at > BLOCK_BYTES or want > BLOCK_BYTES - at)
    {
        // EXHAUSTION IS THE PANIC, and sharing is the alternative it exists to refuse.
        // Raise KICKOS_RX_TLS_BLOCK: the total is the sum of this image's thread_local
        // sizes plus their alignment run-ups, plus 4 + 2 bytes per object for the table.
        rx_tls_panic();
    }
    *brk = static_cast<uint16_t>(at + want);
    table[index] = static_cast<uint16_t>(at);

    unsigned char* const dst = tp + at;
    unsigned char const* const templ = obj->templ;
    if (templ == nullptr)
    {
        // Already zero: tls_seat() zeroed the carve and every offset is handed out once.
        // Written anyway so the guarantee is this function's and not the seater's.
        for (size_t i = 0; i < want; i++)
        {
            dst[i] = 0;
        }
        return dst;
    }
    // The template of a .tdata-class object is __emutls_t.<name> in .rodata, so code
    // flash. Bounded because obj->templ is app-writable: unbounded it is an arbitrary read,
    // and on rx72m-flat there is no MPU to turn that into a fault.
    uintptr_t const t = reinterpret_cast<uintptr_t>(templ);
    uintptr_t const rom_lo = reinterpret_cast<uintptr_t>(__kickos_code_start);
    uintptr_t const rom_hi = reinterpret_cast<uintptr_t>(__kickos_code_end);
    if (t < rom_lo or t > rom_hi - want)
    {
        rx_tls_panic();
    }
    for (size_t i = 0; i < want; i++)
    {
        dst[i] = templ[i];
    }
    return dst;
}

#endif
