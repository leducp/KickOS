# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Host-only: the free list and the reply count are invisible from userspace, and the mint is
# driven on a peer's table.
kickos_add_kseam_gate(NAME cap_probe SOURCES reply_probe.cc)
