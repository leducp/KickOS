// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Configuration split by AUDIENCE into three buckets. Edit the one that matches your
// role; do not re-conflate them.
//   config/limits.h  structural / fixed: design invariants, not knobs
//   config/system.h  user / app:         provisioning knobs, sized per app
//   config/board.h   board / chip:       hardware-derived
// This umbrella includes all three so a plain <kickos/config.h> resolves the
// whole set.

#ifndef KICKOS_CONFIG_H
#define KICKOS_CONFIG_H

#include <kickos/config/limits.h>
#include <kickos/config/system.h>
#include <kickos/config/board.h>

#endif
