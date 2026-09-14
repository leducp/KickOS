// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Base of the run of logical IRQ lines a board leaves free for an app to claim, attach and
// inject into. A chip whose own drivers sit in the default block, or whose controller cannot
// set the pending state of a line that low, declares its own in chip_limits.h; the default
// below is what every other chip takes.
//
// AN APP MAY ASSUME BASE+0 THROUGH BASE+10 and no more: that is the widest block any chip
// here declares free, and the self-test already spends all eleven.

#ifndef KICKOS_SYS_IRQ_FREE_H
#define KICKOS_SYS_IRQ_FREE_H

// The chip's own constants. A sim build ships none (same guard as config/board.h).
#if defined(__has_include) and __has_include(<kickos/chip_limits.h>)
#include <kickos/chip_limits.h>
#endif

#ifndef KICKOS_IRQ_FREE_BASE
#define KICKOS_IRQ_FREE_BASE 6
#endif

#endif // KICKOS_SYS_IRQ_FREE_H
