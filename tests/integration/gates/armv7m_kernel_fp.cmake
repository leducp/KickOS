# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# On armv7m, no FP instruction in any image's kernel text but the switcher's own
# (tests/static/check_image_rules.sh, rule armv7m_kernel_fp).

if(NOT KICKOS_ARCH STREQUAL "armv7m")
  return()
endif()
get_property(_kfp_images GLOBAL PROPERTY KICKOS_EMITTED_IMAGES)
if(NOT _kfp_images)
  message(FATAL_ERROR "armv7m_kernel_fp: this configure emits no image to read")
endif()
foreach(_img IN LISTS _kfp_images)
  kickos_image_rule(armv7m_kernel_fp ${_img})
endforeach()
