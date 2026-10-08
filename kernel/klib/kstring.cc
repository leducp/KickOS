// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// lib/libc/string.cc compiled a second time under the kernel's private names.

#define memcpy kmemcpy
#define memset kmemset
#define memmove kmemmove
#define memcmp kmemcmp
#define strlen kstrlen
#define strnlen kstrnlen

#include "../../lib/libc/string.cc"
