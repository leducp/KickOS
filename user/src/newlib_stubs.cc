// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Newlib-compatible userspace porting layer (the toolchain-libc lesson from
// NuttX): the bottom edge that lets the toolchain's libstdc++/libsupc++ sit on
// top of KickOS by routing its OS calls through KickOS syscalls. NOT compiled
// for the sim (host glibc already provides these symbols); wired in from M1
// when userspace links the ARM toolchain libc.

#include <kickos/sys.h>

#include <stddef.h>
#include <stdint.h>

extern "C" {

int _write(int fd, const char* buf, int len) {
  return static_cast<int>(kos_write(fd, buf, static_cast<size_t>(len)));
}

int _read(int, char*, int) { return 0; }
int _close(int) { return -1; }
int _isatty(int) { return 1; }
int _lseek(int, int, int) { return 0; }
int _fstat(int, void*) { return 0; }
int _getpid(void) { return 1; }
int _kill(int, int) { return -1; }

void _exit(int code) {
  kos_exit(code);
  for (;;) {}
}

// Bump allocator over a fixed userspace heap arena.
static char  s_heap[64 * 1024];
static char* s_brk = s_heap;

void* _sbrk(intptr_t incr) {
  char* prev = s_brk;
  char* next = s_brk + incr;
  if (next < s_heap || next > s_heap + sizeof(s_heap)) {
    return reinterpret_cast<void*>(-1);
  }
  s_brk = next;
  return prev;
}

} // extern "C"
