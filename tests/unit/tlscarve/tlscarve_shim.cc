// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/thread/tls.cc compiled under a forced TLS configuration. The sim build sets
// KICKOS_TLS=0 tree-wide and defines neither KICKOS_TLS_STRIDE nor KICKOS_ARCH_TLS_TCB, so
// without this the target would compile the do-nothing half of the file and gate nothing.
// The arch header is pulled in FIRST so an arch that does state KICKOS_ARCH_TLS_TCB is
// overridden here rather than colliding with the define below.

#include "tlscarve_config.h"

#undef KICKOS_TLS
#define KICKOS_TLS 1

#include <kickos/arch/arch.h>
#include <kickos/config/system.h>

#ifdef KICKOS_ARCH_TLS_TCB
#undef KICKOS_ARCH_TLS_TCB
#endif
#define KICKOS_ARCH_TLS_TCB TLSCARVE_TCB

#ifdef KICKOS_TLS_STRIDE
#undef KICKOS_TLS_STRIDE
#endif
#define KICKOS_TLS_STRIDE TLSCARVE_STRIDE

// The sim build resolves this 0, its arch seating no thread pointer at all, so without the
// override the shim would compile the seating half of tls_stack_admissible for every case
// and the stride arms would gate nothing.
#ifdef KICKOS_TLS_FROM_SP
#undef KICKOS_TLS_FROM_SP
#endif
#define KICKOS_TLS_FROM_SP TLSCARVE_FROM_SP

#include "../../../kernel/thread/tls.cc"

// The image record tls.cc reads, as the link's script states it from the four symbols a case
// fabricates: one image, whose part is the whole segment, and whose libc state carve is
// TLSCARVE_REENT_CARVE. A case that states images of its own defines TLSCARVE_IMAGES and its own
// image_record.
#ifndef TLSCARVE_IMAGES
extern "C"
{
    extern unsigned char __kickos_tdata_start[];
    extern unsigned char __kickos_tdata_end[];
    extern unsigned char __kickos_tbss_start[];
    extern unsigned char __kickos_tbss_end[];
}

namespace kickos
{
    kos_image_header const& image_record(size_t)
    {
        static kos_image_header h;
        size_t const tdata = static_cast<size_t>(__kickos_tdata_end - __kickos_tdata_start);
        size_t const tbss = static_cast<size_t>(__kickos_tbss_end - __kickos_tbss_start);
        h.tls_template = reinterpret_cast<uintptr_t>(__kickos_tdata_start);
        h.tls_offset = 0;
        h.reent_carve = TLSCARVE_REENT_CARVE;
        h.tls_init = tdata;
        h.tls_total = 0;
        if (tdata + tbss != 0)
        {
            h.tls_total = static_cast<uintptr_t>(__kickos_tbss_end - __kickos_tdata_start);
        }
        return h;
    }
}
#endif
