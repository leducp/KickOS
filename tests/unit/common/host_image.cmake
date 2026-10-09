# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_host_image(<target> TEXT <symbol> <bytes> DATA <symbol> <bytes>)
#   Places the app image bounds the kernel links against on two host arrays of the gate's own,
#   a zero size giving an empty extent.
function(kickos_host_image target)
  cmake_parse_arguments(HI "" "" "TEXT;DATA" ${ARGN})
  list(GET HI_TEXT 0 _text)
  list(GET HI_TEXT 1 _text_bytes)
  list(GET HI_DATA 0 _data)
  list(GET HI_DATA 1 _data_bytes)
  target_link_options(${target} PRIVATE -no-pie
    -Wl,--defsym=__kickos_app_rom_start=${_text}
    -Wl,--defsym=__kickos_app_rom_end=${_text}+${_text_bytes}
    -Wl,--defsym=__kickos_app_sram_start=${_data}
    -Wl,--defsym=__kickos_app_sram_end=${_data}+${_data_bytes}
    -Wl,--defsym=__kickos_app_load_delta=0)
endfunction()
