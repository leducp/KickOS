# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# TLS carve gate for kernel/thread/tls.cc. Host-only of necessity: the four .tdata/.tbss
# bounds are linker symbols, so a target image only ever presents the ONE layout its own
# link produced, and neither a linker pad between the two sections nor an empty pair can be
# staged there. A host program fabricates the layout it wants in a data section and reads
# the block size that follows.
#
# ONE EXECUTABLE PER LAYOUT AND PER POSTURE, because the layout is fixed at link time and the
# posture is an #if inside the kernel source the shim compiles:
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
    KERNEL
    LIBRARIES kickos_kseam)
endforeach()

# `seat` differs from the two above in ONE clause, TLSCARVE_FROM_SP 0: the arch that SEATS the
# thread pointer from the context. Same fabrication, same stride, same reserve, so what it
# reports is attributable to that clause.
kickos_add_unit_test(NAME tls_carve_seat
  SOURCES tlscarve_shim.cc tlscarve_seat.cc
  KERNEL
  LIBRARIES kickos_kseam
  DEFINITIONS TLSCARVE_FROM_SP=0)

# `idle` is a masking arch with a thread_local declared above one kernel core: every core's idle
# is refused by admission yet must still be created.
kickos_add_unit_test(NAME tls_carve_idle
  SOURCES tlscarve_shim.cc tlscarve_idle.cc
  LIBRARIES kickos_kseam_smp
  DEFINITIONS TLSCARVE_STRIDE=4096u)

# `reent` is a seating arch with a libc state carve and no thread_local, two kernel cores: a pool
# thread's carve is the state alone and every idle's is nothing. `below` is a masking arch with a
# thread_local: the template is seated past the state, which the carve leaves unwritten.
kickos_add_unit_test(NAME tls_carve_reent
  SOURCES tlscarve_shim.cc tlscarve_reent.cc
  LIBRARIES kickos_kseam_smp
  DEFINITIONS TLSCARVE_REENT_CARVE=96u TLSCARVE_STRIDE=4096u TLSCARVE_FROM_SP=0)
kickos_add_unit_test(NAME tls_carve_below
  SOURCES tlscarve_shim.cc tlscarve_below.cc
  LIBRARIES kickos_kseam
  DEFINITIONS TLSCARVE_REENT_CARVE=96u)

# Two image records over one segment: each image's block, template and thread pointer bias.
kickos_add_unit_test(NAME tls_carve_images
  SOURCES tlscarve_shim.cc tlscarve_images.cc
  LIBRARIES kickos_kseam
  DEFINITIONS TLSCARVE_IMAGES=1 TLSCARVE_FROM_SP=0)
