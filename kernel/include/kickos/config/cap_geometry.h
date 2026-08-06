// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The capability-table storage granule and the off-pool run count: the two structural
// constants cmake/cap_table.cmake must read while it is still computing the table width, so
// nothing here may depend on a configured value.
//
// MACROS, not constexpr, and that is the whole point: CMake reads these through the
// preprocessor, and a constexpr is invisible to it. KCAP_RUN_OFF_POOL used to be a constexpr
// in cap.h and cost a file(STRINGS) regex to scrape back out, whose own comment recorded that
// the trailing semicolon was load-bearing. The rule: a constant that ASSEMBLY or CMAKE must
// read is a macro; everything else stays a typed constant.

#ifndef KICKOS_CONFIG_CAP_GEOMETRY_H
#define KICKOS_CONFIG_CAP_GEOMETRY_H

// armv6m has no divide instruction, so the chunk width must stay a power of two. Changing it
// also moves KCAP_CHUNK_SHIFT (cap.h), which boards compile the flat decode, and so the
// cap_chunk_span PARTIAL list in user/apps/common/selftest/CMakeLists.txt.
#define KCAP_CHUNK_TARGET 8

// The runs held by something that is NOT a thread-pool slot: root's (a static TCB, so no slot
// accounts for it) and the one thread_spawn holds in its ThreadAttr until thread_create takes
// it over. idle holds none. A new kind of holder is a term here.
#define KCAP_RUN_OFF_POOL 2

#endif
