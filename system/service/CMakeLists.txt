# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The stock services a composition names as a program, each an executable of its own.
if(KICKOS_BUILD_APPS AND KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  add_subdirectory(amprelay)
endif()
