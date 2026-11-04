// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// user/src/newlib_stubs.cc with every name the host libc also defines renamed, since the real
// ones would override it for GoogleTest too; the aliases name their targets by string, so the
// underscored names stay. The system headers come first, so the renames reach only the stubs.

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

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
#define clock_gettime kos_ut_clock_gettime
#define clock_settime kos_ut_clock_settime
#define nanosleep kos_ut_nanosleep
#define __dso_handle kos_ut_dso_handle

#include "../../../user/src/newlib_stubs.cc"
