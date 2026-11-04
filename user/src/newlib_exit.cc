// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Every link wraps exit(), so an image that calls it links this object and libc's exit() behind
// it; one that never calls it links neither.

#include <kickos/reent.h>

extern "C"
{
void __real_exit(int code) __attribute__((noreturn));
void __wrap_exit(int code) __attribute__((noreturn));

// The caller's thread_local objects are destroyed before anything atexit registered runs.
void __wrap_exit(int code)
{
    kickos_thread_dtors_run();
    __real_exit(code);
}
}
