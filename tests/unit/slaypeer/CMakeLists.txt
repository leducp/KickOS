# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A slay whose victim is RUNNING on a PEER core, over the K-seam sources AT TWO KERNEL CORES.
# Those are already compiled by tests/unit/exitquiesce as kickos_kseam_smp, whose target must
# therefore exist before this directory is added.
#
# The core counts AND THE MODEL reach C through the root's add_compile_definitions, which this
# directory INHERITS, so each is dropped from the inherited copy rather than shadowed: two -D
# of one macro with different values is a redefinition -Werror refuses. The object library
# carries the values this gate compiles against.
get_directory_property(_slaypeer_dirdefs COMPILE_DEFINITIONS)
list(FILTER _slaypeer_dirdefs EXCLUDE
     REGEX "^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED)=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_slaypeer_dirdefs}")

add_executable(peer_slay peer_slay.cc)
target_link_libraries(peer_slay PRIVATE kickos_kseam_smp GTest::gtest_main)
target_compile_options(peer_slay PRIVATE ${KICKOS_WARN_FLAGS})
kickos_discover_unit_tests(peer_slay)
