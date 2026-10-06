# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Sustained-output witness for a published USB CDC console, the packaged driver its composition
# names as stdout.
add_executable(usbcdcwit main.cc)
kickos_app_system(usbcdcwit COMPOSITION systems/${KICKOS_BOARD}.yaml)
kickos_emit_image(usbcdcwit)
kickos_app_judge(usbcdcwit tests/integration/check_usbcdcwit.sh)
