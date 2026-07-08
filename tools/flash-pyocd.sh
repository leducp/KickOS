#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# flash-<tool> backend: flash via pyOCD (CMSIS-DAP/DAPLink). Usage: <board> [app]
set -euo pipefail
FL_ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$FL_ROOT/tools/flash-common.sh"
flash_resolve "$@"
have pyocd || die "pyocd not on PATH"
case "$FL_CHIP" in nrf51) t=nrf51 ;; mk64f) t=k64f ;; *) t=$FL_CHIP ;; esac   # pyocd target
run pyocd flash -t "$t" "$FL_HEX"
