# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The interrupt entry against a concurrent teardown. kernel/irq/irq.cc is compiled here as the
# real kernel translation unit, and irq_seam.cc answers its whole undefined set.
#
# AT TWO KERNEL CORES, which is the point and is why this gate builds its own object rather
# than joining the K-seam fixture: that library takes the preset's core count, and the sim
# preset drives one core, where the mechanism under test compiles to nothing.
#
# The core count reaches C through the root's add_compile_definitions, which this directory
# INHERITS, so it is dropped from the inherited copy rather than shadowed: two -D of one macro
# with different values is a redefinition -Werror refuses.
# TWO PLACES CARRY IT: this directory inherited the root's copy, and kickos_kernel's own
# COMPILE_DEFINITIONS was seeded from the same place when that target was created.
get_directory_property(_irqq_dirdefs COMPILE_DEFINITIONS)
list(FILTER _irqq_dirdefs EXCLUDE REGEX "^KICKOS_(NUM|KERNEL)_CORES=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_irqq_dirdefs}")

kickos_add_unit_test(NAME irq_quiesce
  SOURCES quiesce.cc irq_seam.cc
          "${PROJECT_SOURCE_DIR}/kernel/irq/irq.cc"
          "${PROJECT_SOURCE_DIR}/kernel/sync/klock.cc"
  INCLUDES "$<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>"
  DEFINITIONS "$<FILTER:$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>,EXCLUDE,^KICKOS_(NUM|KERNEL)_CORES=>"
              KICKOS_NUM_CORES=2
              KICKOS_KERNEL_CORES=2
              KICKOS_MULTICORE_MODEL_SHARED=1)
