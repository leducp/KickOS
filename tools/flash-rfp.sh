#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# flash-<tool> backend: flash a Renesas RX board via the Renesas Flash Programmer
# CLI (rfp-cli). Usage: tools/flash-rfp.sh <board> [app]   (app default: hello)
# Like every backend, rfp-cli must be on PATH (symlink the Renesas install if needed).
#
# Loads the .hex (Intel HEX carries its own addresses incl. reset vector + option
# memory). Flags (verified on real silicon):
#   -tool e2l -if fine   E2 Lite emulator over the FINE 1-wire interface
#   -auth id FF..FF      all-FF ID authenticates blank/unlocked flash
#   -a                   erase + program + verify
#   -run                 REQUIRED: releases reset so the core actually runs
# Do NOT pass -osc: it triggers an input-frequency error; rfp's default is correct.
set -euo pipefail
FL_ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$FL_ROOT/tools/flash-common.sh"
flash_resolve "$@"
have rfp-cli || die "rfp-cli not on PATH"
[ -e "$FL_HEX" ] || die "no $FL_HEX (build it: cmake --build build/$FL_BOARD --target $FL_APP)"

case "$FL_BOARD" in
    rx72m) dev=RX72x ;;
    *) die "flash-rfp: no rfp -device string for board '$FL_BOARD' (add a row)" ;;
esac

say "$FL_BOARD [$dev] <- $FL_HEX (erase+program+verify, then run)"
run rfp-cli -device "$dev" -tool e2l -if fine -auth id FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -run -a "$FL_HEX"
