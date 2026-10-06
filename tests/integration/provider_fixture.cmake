# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CMAKE_PROJECT_INCLUDE for check_provider_alias.sh: a user-named pin map, defined before the
# tree names its link group, given its KickOS:: name only where KICKOS_FIXTURE_ALIAS is set.

include("${CMAKE_SOURCE_DIR}/cmake/kickos.cmake")
add_library(kickos_pinmap_fixture STATIC "${CMAKE_SOURCE_DIR}/system/init/common/pinmap_none.cc")
if(KICKOS_FIXTURE_ALIAS)
  kickos_alias_target(kickos_pinmap_fixture)
endif()
