# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The cross-core ask a re-placement owes, over the K-seam sources AT TWO KERNEL CORES. Those
# are already compiled by tests/unit/exitquiesce as kickos_kseam_smp, whose target must
# therefore exist before this directory is added.
#
# The core counts AND THE MODEL reach C through the root's add_compile_definitions, which
# this directory INHERITS, so each is dropped from the inherited copy rather than shadowed:
# two -D of one macro with different values is a redefinition -Werror refuses. The object
# library carries the values this gate compiles against.
get_directory_property(_mask_dirdefs COMPILE_DEFINITIONS)
list(FILTER _mask_dirdefs EXCLUDE REGEX "^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED)=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_mask_dirdefs}")

add_executable(migrate_ask migrate_ask.cc)
target_link_libraries(migrate_ask PRIVATE kickos_kseam_smp GTest::gtest_main)
target_compile_options(migrate_ask PRIVATE ${KICKOS_WARN_FLAGS})
kickos_discover_unit_tests(migrate_ask)
