#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The out-of-tree MCU example run: install the build's package in a scratch prefix, build
# examples/oot-mcu-app against it with the package's toolchain file, and boot it under
# QEMU_MACHINE. It prints its line and its main's status ends the system, through the default
# composition. check_oot_export_mcu.sh holds the packaging claims on the same example.
#
#   check_oot_mcu_run.sh <kickos-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: check_oot_mcu_run.sh <kickos-build> <kickos-source> <cmake>"
KICKOS_BUILD="${1:?$USAGE}"
KICKOS_SRC="${2:?$USAGE}"
CMAKE="${3:?$USAGE}"
need_qemu_machine

# Must match examples/oot-mcu-app/main.cc.
APP_LINE='[oot-mcu] hello from an out-of-tree bare-metal KickOS app'
APP_STATUS=42

scratch_dir

echo "== building oot_mcu_app against the installed package =="
package_image "$KICKOS_BUILD" "$CMAKE" "$KICKOS_SRC/examples/oot-mcu-app" oot_mcu_app

echo "== running oot_mcu_app through the default composition =="
"$(dirname "$0")/check_system_default.sh" "$IMAGE" "$APP_LINE" "$APP_STATUS"
exit $?
