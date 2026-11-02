# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console's transmission-complete wait, witnessed against a tail it must not cut. It drives
# UART0 and the TX pad directly, which only the flat build lets a task do; the parent builds it
# there.
add_executable(c6txidle main.cc)
target_link_libraries(c6txidle PRIVATE KickOS::kickos KickOS::system_default)
# The app reads the probe's record, a kernel word, which only a board enforcing nothing lets it.
set_target_properties(c6txidle PROPERTIES KICKOS_IMAGE_KERNEL_NAMES kickos_c6_txidle_record)
kickos_emit_image(c6txidle)
kickos_app_judge(c6txidle tests/integration/check_c6txidle.sh)
