// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The libc reentrant-state seam. The kernel never names struct _reent and includes no newlib
// header: the image header (<kickos/sys/image.h>) STATES where libc's own state lives, as plain
// addresses and a width.
//
// The sim's libc is the HOST's and owns its own state, so nothing below is compiled there; do
// NOT answer that posture with an empty descriptor, whose seat word is an address the kernel
// would still write through.
//
// Each pool thread's state is the first KICKOS_REENT_CARVE bytes of its stack block, below its
// TLS block (kickos/tls.h). The carve ends in the CELL, two pointer-wide words that end where
// the TLS block starts: the state's address, then the thread's lock id (kickos/lock_id.h). Both
// are written once at thread creation. At KICKOS_REENT_PER_THREAD 1 libc calls __getreent,
// which reads the cell below the block, the thread pointer plus the image's offset in the
// thread-local segment. At 0 libc reads ONE word per image, `seat`, which the kernel
// rewrites at every switch, correct only while one core runs the threads that share it; the
// lock id is then read through that word.
//
// The seam is DATA: where a translating backend splits the image every EL0-reachable leaf
// carries privileged-execute-never, so the kernel may not call app text at all.
//
// EVERY WRITE IS A kmemcpy AND NEVER A TYPED STORE. struct _reent is in scope only on the user
// side, so a kernel store through void** asserts an effective type the object does not have.
//
// The header's fields are read in place from each image's published record.

#ifndef KICKOS_REENT_H
#define KICKOS_REENT_H

#include <stddef.h>
#include <stdint.h>

// Global scope: an elaborated `struct arch_aspace*` first seen inside namespace kickos would
// declare a second, unrelated type.
struct arch_aspace;

#if KICKOS_LIBC_REENT

extern "C"
{
    // User side, defined only where libc builds per-thread scratch on the heap on first use.
    // The exit stub runs it before the trap because nothing after that can: the kernel reaches
    // neither the heap nor the allocator, and the next prime overwrites the pointers.
    void kickos_reent_release(void);
}

struct kos_image_header;

namespace kickos
{
    // Must precede the first thread_create, over image `image`'s published header, each image in
    // turn. Panics on a state wider than the carve leaves below its cell.
    void reent_seam_read(size_t image, kos_image_header const& h);

    // The state idle runs on: image 0's, the image of the task the boot seats it in.
    void* reent_shared_state(void);

    // Writes the cell of a pool thread whose stack block the kernel reaches at `home`, whose
    // state is `state` in the thread's own view.
    void reent_cell_seat(void* home, void* state, uint32_t self);

    // SWITCH-IN AND NOWHERE ELSE, with kernel().current already the INCOMING thread: it writes
    // hundreds of bytes into memory that thread owns. `space` is its own, reached through the
    // kaccess seam; null with no translating backend.
    //
    // A REFUSED WRITE SLAYS THE INCOMING THREAD AND MUST NOT PANIC: switch_book's claim three
    // statements past the call redirects the resume.
    // On sched::start's path nothing claims a resume, so a kill lands at its next syscall entry.
    // The state is primed from image `image`'s, the thread's own.
    void reent_prime(struct arch_aspace* space, size_t image, void* state);

#if not KICKOS_REENT_PER_THREAD
    // Runs on EVERY switch, into `seat`, the word the incoming thread's task caches for its
    // image (Task::reent_seat).
    void reent_seat(void* seat, void* state);
#endif
}

#endif

#endif
