# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# TLS carve gate for kernel/thread/tls.cc. Host-only of necessity: the four .tdata/.tbss
# bounds are linker symbols, so a target image only ever presents the ONE layout its own
# link produced, and neither a linker pad between the two sections nor an empty pair can be
# staged there. A host program fabricates the layout it wants in a data section and reads
# the block size that follows.
#
# TWO EXECUTABLES, because the layout is fixed at link time and one binary can only carry
# one set of the four symbols:
#
#   span   .tdata, a 16-byte pad, .tbss: the block must be sized by the SPAN, since the
#          compiler's thread_local offsets are relative to the layout the linker produced,
#          pad included, and the sum would carve short by that pad
#   empty  no thread_local anywhere, where .tbss lands four bytes BELOW .tdata and only the
#          two section sizes can decide emptiness
#
# Takes the kernel target's include path and definitions verbatim: the shim compiles a
# kernel source, so a divergent config here would gate a different translation unit than the
# one that ships.
foreach(_case span empty)
  kickos_add_unit_test(NAME tls_carve_${_case}
    SOURCES tlscarve_shim.cc tlscarve_${_case}.cc
    INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
    DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
endforeach()
