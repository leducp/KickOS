# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The manifest's open-ended field sets, which tools/manifest/genmanifest.py writes from and
# manifest.py reads against. Standard library only: the writer runs under the configure's Python,
# outside the tool's uv environment.

import re

# Every bound the kernel build states, by name rather than by list, so a new knob is exported where
# it is declared, and the range table a reservation spends. KICKOS_MAX_THREAD_WINDOWS is the
# protection unit's region budget and sits there.
POOL = re.compile(r"KICKOS_(MAX_[A-Z0-9_]+|ASPACE_RANGES)")
WINDOWS_KNOB = "KICKOS_MAX_THREAD_WINDOWS"
# A kind's default budget, KICKOS_BUDGET_<KIND> for each kind of <kickos/sys/budget.h>.
BUDGET_KNOB = "KICKOS_BUDGET_%s"
# One kind of <kickos/sys/budget.h>'s KOS_BUDGET_KINDS: X(NAME, key, pool).
BUDGET_KIND = re.compile(r"^\s*X\(\s*([A-Z0-9_]+)\s*,\s*([a-z0-9_]+)\s*,\s*([a-z0-9_]+)\s*\)\s*\\?\s*$")

DRIVER_FIELDS = ("windows", "lines", "triggers", "entry", "threads", "endpoints", "notifications", "block",
                 "block_cache", "posture", "barrier", "console", "usb_device", "start", "receiver", "client",
                 "slots", "authority")
# A catalogue entry may leave these out.
DRIVER_OPTIONAL = ("authority",)


def is_pool(name):
    return POOL.fullmatch(name) is not None and name != WINDOWS_KNOB


def budget_kinds(header):
    """The kinds <kickos/sys/budget.h> lists, in its order, as (NAME, key, pool)."""
    kinds = []
    with open(header, encoding="utf-8") as stream:
        for line in stream:
            match = BUDGET_KIND.match(line)
            if match is not None:
                kinds.append(match.groups())
    return kinds
