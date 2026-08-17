# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The probe that stands in for the reply mint's check. Host-only: the free list and the reply
# count are invisible from userspace, and the mint is driven on a PEER's table.
kickos_add_kseam_gate(NAME cap_probe SOURCES reply_probe.cc)
