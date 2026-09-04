# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The core-mask admission, AT FOUR KERNEL CORES WITH TWO OF THEM ISOLATED. No preset carries
# that shape: qemu-arm64-smpiso isolates ONE core, and a mask naming SEVERAL isolated cores, the
# pool an explicit affinity reserves, needs two.
#
# Header-only: sched.h carries the whole rule, so this gate compiles no kernel source and the
# core count reaches it through definitions alone.
#
# The core counts, the model AND the isolation mask reach C through the root's
# add_compile_definitions, which this directory INHERITS, so each is dropped from the inherited
# copy rather than shadowed: two -D of one macro with different values is a redefinition
# -Werror refuses.
get_directory_property(_admit_dirdefs COMPILE_DEFINITIONS)
list(FILTER _admit_dirdefs EXCLUDE
     REGEX "^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|ISOLATED_CORES)=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_admit_dirdefs}")

add_executable(admit_mask admit_mask.cc)
target_compile_features(admit_mask PRIVATE ${KICKOS_CXX_STANDARD})
target_include_directories(admit_mask PRIVATE
  $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(admit_mask PRIVATE
  "$<FILTER:$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>,EXCLUDE,^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|ISOLATED_CORES)=>"
  KICKOS_NUM_CORES=4
  KICKOS_KERNEL_CORES=4
  KICKOS_MULTICORE_MODEL_SHARED=1
  KICKOS_ISOLATED_CORES=0xC)
target_link_libraries(admit_mask PRIVATE GTest::gtest_main)
target_compile_options(admit_mask PRIVATE ${KICKOS_WARN_FLAGS})
kickos_discover_unit_tests(admit_mask)
