// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The capability-table storage granule, and nothing else. cmake/cap_table.cmake preprocesses
// this header while it is still computing the table width, so nothing here may depend on a
// configured value.

#ifndef KICKOS_CONFIG_CAP_GEOMETRY_H
#define KICKOS_CONFIG_CAP_GEOMETRY_H

// armv6m has no divide instruction, so the chunk width must stay a power of two. Changing it
// also moves KCAP_CHUNK_SHIFT (cap.h), which boards compile the flat decode, and so the
// cap_chunk_span PARTIAL list in user/apps/common/selftest/CMakeLists.txt.
#define KCAP_CHUNK_TARGET 8

#endif
