// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// lib/libc/fmt.cc compiled a second time under the names kickos/kruntime.h declares. The
// app keeps kvsnprintf/ksnprintf: those two are called from both sides, and a global
// symbol has one value.

#define kvsnprintf kfmt_vsnprintf
#define ksnprintf kfmt_snprintf

#include "../../lib/libc/fmt.cc"
