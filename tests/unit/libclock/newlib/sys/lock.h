// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's retargetable lock interface, as its sys/lock.h declares it.

#ifndef KICKOS_TESTS_UNIT_LIBCLOCK_NEWLIB_SYS_LOCK_H
#define KICKOS_TESTS_UNIT_LIBCLOCK_NEWLIB_SYS_LOCK_H

extern "C"
{
struct __lock;
typedef struct __lock* _LOCK_T;

extern struct __lock __lock___sfp_recursive_mutex;
extern struct __lock __lock___atexit_recursive_mutex;
extern struct __lock __lock___at_quick_exit_mutex;
extern struct __lock __lock___malloc_recursive_mutex;
extern struct __lock __lock___env_recursive_mutex;
extern struct __lock __lock___tz_mutex;
extern struct __lock __lock___dd_hash_mutex;
extern struct __lock __lock___arc4random_mutex;

void __retarget_lock_init(_LOCK_T* lock);
void __retarget_lock_init_recursive(_LOCK_T* lock);
void __retarget_lock_close(_LOCK_T lock);
void __retarget_lock_close_recursive(_LOCK_T lock);
void __retarget_lock_acquire(_LOCK_T lock);
void __retarget_lock_acquire_recursive(_LOCK_T lock);
int __retarget_lock_try_acquire(_LOCK_T lock);
int __retarget_lock_try_acquire_recursive(_LOCK_T lock);
void __retarget_lock_release(_LOCK_T lock);
void __retarget_lock_release_recursive(_LOCK_T lock);
}

#endif
