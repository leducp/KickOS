# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# cmake -DSOURCE=<dir> -P version_probe.cmake: prints `<version>|<package>|<inputs>` for <dir>.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/kickos_version.cmake")
kickos_version("${SOURCE}" _v _p _i)
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_v}|${_p}|${_i}")
