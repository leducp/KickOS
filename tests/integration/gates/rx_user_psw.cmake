# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# On RX, no image this configure emits masks interrupts in its user text, where the mask is
# ignored (tests/static/check_image_rules.sh, rule rx_user_psw).

if(NOT KICKOS_ARCH STREQUAL "rxv3")
  return()
endif()
get_property(_rx_user_psw_images GLOBAL PROPERTY KICKOS_EMITTED_IMAGES)
if(NOT _rx_user_psw_images)
  message(FATAL_ERROR "rx_user_psw: this configure emits no image to read")
endif()
foreach(_img IN LISTS _rx_user_psw_images)
  kickos_image_rule(rx_user_psw ${_img})
endforeach()
