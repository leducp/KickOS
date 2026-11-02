// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// errnoprobe's arm F: libc on a core's first thread, before any switch. Root is a core's first
// thread, entered by arch_start with no switch, so only the FS base that path wrote can hand
// libc this thread's state. Root runs its image's constructors before the init it then runs
// switches anything in, so the arm is one: strtok and strtol out of newlib, then which core
// entered root. Every image holds this object, root's the init's, and root prints the line
// itself: main's space runs the same constructor on a thread a switch entered.
// Not excluded: an interrupt switching root out and back in before the reads.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/image.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>

KOS_IMAGE_BOTH_OBJECT()

#if defined(__x86_64__) and not KICKOS_ARCH_SIM
namespace
{
    __attribute__((constructor)) void arm_first_thread()
    {
        char buf[] = "kick os";
        char const* const t0 = strtok(buf, " ");
        char const* const t1 = strtok(nullptr, " ");
        errno = 0;
        char* end = nullptr;
        (void)strtol("99999999999999999999999999", &end, 10);
        int const e = errno;
        struct _reent* const r = __getreent();
        uintptr_t cell = 0;
        __asm__ volatile("movq %%fs:-16, %0" : "=r"(cell));
        int const core = kos_core_current();

        char const* verdict = "ok";
        if (t0 == nullptr or t1 == nullptr or strcmp(t0, "kick") != 0 or strcmp(t1, "os") != 0)
        {
            verdict = "STRTOK LOST ITS PLACE";
        }
        else if (e != ERANGE)
        {
            verdict = "STRTOL DID NOT SET ERRNO";
        }
        else if (reinterpret_cast<uintptr_t>(r) != cell)
        {
            verdict = "__getreent IS NOT THE FS CELL'S WORD";
        }
        else if (r == _GLOBAL_REENT)
        {
            verdict = "LIBC RAN ON THE PROCESS-WIDE STATE";
        }
        else if (core < 0)
        {
            verdict = "THE CORE IS UNREADABLE";
        }
        // Root's handle is the bare 0: slot 0, generation 0, never reclaimed.
        if (kos_thread_self() != 0)
        {
            return;
        }
        char line[96];
        ksnprintf(line, sizeof(line), "[errnoprobe] F first core %d %s\n", core, verdict);
        kos::print(line);
    }
}
#endif
