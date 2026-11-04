// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_USER_APPS_COMMON_CXXTEST_THREADS_H
#define KICKOS_USER_APPS_COMMON_CXXTEST_THREADS_H

// Before any other throw in the image: the first one sorts the unwinder's frame registry.
void cxxtest_first_throws(void (*report)(char const*, bool));
void cxxtest_threads(void (*report)(char const*, bool));

#endif
