# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The switch_to redirect hook: WHICH context it rebuilds, WHEN, and with what. Host-only of
# necessity in both directions: the hook has no userspace-visible effect until a real
# context is resumed, and a resumed context is exactly what this seam cannot produce, so the
# arguments and the ordering are all there is to gate here. The death point itself is
# witnessed on qemu, qemu-riscv and the fleet.
kickos_add_kseam_gate(NAME slay_hook SOURCES hook.cc)
