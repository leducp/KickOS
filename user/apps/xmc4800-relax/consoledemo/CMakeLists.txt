# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Console bring-up demo: its composition names the packaged xmcuart as stdout, and main and a
# worker print through it. Judged from a silicon capture.
add_executable(consoledemo main.cc)
kickos_app_system(consoledemo COMPOSITION system.yaml)
kickos_emit_image(consoledemo)
kickos_app_judge(consoledemo tests/integration/check_consoledemo.sh)
