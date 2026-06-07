// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Configuration split by AUDIENCE (architecture.md 8e) into three buckets --
// edit the one that matches your role, don't re-conflate them:
//   config/limits.h  structural / fixed  -- design invariants, not knobs
//   config/system.h  user / app          -- provisioning knobs, sized per app
//   config/board.h   board / chip        -- hardware-derived (moves to the board
//                                            layer at M1/M2)
// This umbrella includes all three so a plain <kickos/config.h> resolves the
// whole set.

#ifndef KICKOS_CONFIG_H
#define KICKOS_CONFIG_H

#include <kickos/config/limits.h>
#include <kickos/config/system.h>
#include <kickos/config/board.h>

#endif
