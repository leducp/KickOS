// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// user/src/newlib_stubs.cc with every name the host libc also defines renamed, since the real
// ones would override it for GoogleTest too; the aliases name their targets by string, so the
// underscored names stay. The system headers come first, so the renames reach only the stubs.

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>

#define _exit kos_ut_exit
#define _fini kos_ut_fini
#define write kos_ut_plain_write
#define read kos_ut_plain_read
#define close kos_ut_plain_close
#define isatty kos_ut_plain_isatty
#define lseek kos_ut_plain_lseek
#define fstat kos_ut_plain_fstat
#define getpid kos_ut_plain_getpid
#define kill kos_ut_plain_kill
#define gettimeofday kos_ut_plain_gettimeofday
#define getentropy kos_ut_plain_getentropy
#define kos_clock_set_realtime kos_ut_clock_set_realtime
#define __dso_handle kos_ut_dso_handle
#define __malloc_lock kos_ut_malloc_lock
#define __malloc_unlock kos_ut_malloc_unlock

#include "../../../user/src/newlib_stubs.cc"
