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
// Each pool thread's state is the first tls_libc_carve(image) bytes of its stack block, below its
// TLS block (kickos/tls.h), sized by the newlib profile its image links. The carve ends in the
// CELL, KICKOS_REENT_CELL_WORDS pointer-wide words that end where the TLS block starts: the user
// runtime's own words, then the state's address, then the thread's lock id (kickos/lock_id.h). All
// are written once at thread creation, the runtime's words zero. At KICKOS_REENT_PER_THREAD 1 libc
// calls __getreent, which reads the cell below the block, the thread pointer plus the image's
// offset in the thread-local segment. At 0 libc reads ONE word per image, `seat`, which the kernel
// rewrites at every switch, correct only while one core runs the threads that share it; the lock id
// is then read through that word.
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

// The C++ runtime's per-thread exception state, its __cxa_eh_globals: the caught chain and the
// uncaught count, and on the ARM EH ABI the propagating chain. CMakeLists.txt counts the same.
#if defined(__arm__)
#define KICKOS_EH_GLOBALS_WORDS 3
#else
#define KICKOS_EH_GLOBALS_WORDS 2
#endif

// User side: the running thread's own words in its cell, the threads layer's key values, the
// generation of the key each was set under, the onces it is running, its exception state and
// the head of its thread_local destructor list.
struct KickosThreadWords
{
    void* keys[KICKOS_THREAD_KEYS];
    uintptr_t key_gens[KICKOS_THREAD_KEYS];
    void* once;
    void* eh_globals[KICKOS_EH_GLOBALS_WORDS];
    void* thread_dtors;
};
static_assert(sizeof(KickosThreadWords) == (KICKOS_REENT_CELL_WORDS - 2) * sizeof(void*),
              "the cell is the runtime's words, the state's address and the lock id");

extern "C"
{
    KickosThreadWords* kickos_thread_words(void);
    // Runs the ending thread's key destructors (user/src/gthr.cc); a no-op stands in where no
    // key exists.
    void kickos_keys_thread_end(void);
    // Runs the calling thread's thread_local destructors (user/src/thread_atexit.cc); a no-op
    // stands in where no thread_local registers one.
    void kickos_thread_dtors_run(void);

    // User side: frees the scratch libc built on the heap into the calling thread's state, once
    // kickos_reent_heap_used has run. The exit stub runs it before the trap because nothing
    // after that can: the kernel reaches neither the heap nor the allocator, and the next prime
    // overwrites the pointers.
    void kickos_reent_release(void);
    // User side: the heap seat (user/src/newlib_sbrk.cc) runs it before the image's first
    // allocation, so only an image that allocates links the release's allocator calls.
    void kickos_reent_heap_used(void);
}

struct kos_image_header;

namespace kickos
{
    // Must precede the first thread_create, over image `image`'s published header, each image in
    // turn. Panics on a carve that is off the stack alignment or leaves the state no room below
    // its cell.
    void reent_seam_read(size_t image, kos_image_header const& h);

    // The state idle runs on: image 0's, the image of the task the boot seats it in.
    void* reent_shared_state(void);

    // Writes the cell of a pool thread whose stack block the kernel reaches at `home`, whose
    // state is `state` in the thread's own view and whose image's carve is `carve`.
    void reent_cell_seat(void* home, void* state, uint32_t self, size_t carve);

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
