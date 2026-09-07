# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Root's retired pool slot: the skip, the identity that rides on it, the children's tag, and
# the capability directory a slot that is never reclaimed would otherwise strand. Host-only
# and single core: every claim is pool arithmetic, and the pool is not per-core.
kickos_add_kseam_gate(NAME root_retire SOURCES root_retire.cc)
