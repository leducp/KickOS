// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/mem/ramown.cc and kernel/init/instance.cc at the posture owner_instances.h sets. The
// kernel's runtime header and the host libc's declare the string functions differently, so no
// gtest header shares this TU.

#include "owner_instances.h"

#include "../../../kernel/mem/ramown.cc"
#include "../../../kernel/init/instance.cc"
