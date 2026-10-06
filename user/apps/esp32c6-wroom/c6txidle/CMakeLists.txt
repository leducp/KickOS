# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console's transmission-complete wait, witnessed against a tail it must not cut. It drives
# UART0 and the TX pad directly, which only the flat build lets a task do; the parent builds it
# there.
add_executable(c6txidle main.cc)
target_link_libraries(c6txidle PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(c6txidle)
kickos_app_judge(c6txidle tests/integration/check_c6txidle.sh)
