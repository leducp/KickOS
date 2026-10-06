# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The manifest's open-ended field sets, which tools/manifest/genmanifest.py writes from and
# manifest.py reads against. Standard library only: the writer runs under the configure's Python,
# outside the tool's uv environment.

import re

# Every pool and per-task budget, by name rather than by list, so a new knob is exported where it
# is declared, and the two record tables a reservation spends. KICKOS_MAX_THREAD_WINDOWS is the
# protection unit's region budget and sits there.
POOL = re.compile(r"KICKOS_(MAX_[A-Z0-9_]+|TASK_[A-Z0-9_]+_BUDGET|CAP_TABLE_SUPPLY|RAM_OWNER_SLOTS|ASPACE_RANGES)")
WINDOWS_KNOB = "KICKOS_MAX_THREAD_WINDOWS"

DRIVER_FIELDS = ("windows", "lines", "threads", "endpoints", "notifications", "block", "block_cache", "posture",
                 "barrier", "console", "usb_device", "start", "receiver", "client")


def is_pool(name):
    return POOL.fullmatch(name) is not None and name != WINDOWS_KNOB
