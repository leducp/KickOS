# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# KICKOS_USER_HEAP_SIZE, the link symbol the chip scripts size .userheap by, which an image's
# system target defines from its composition's `heap`.

# kickos_heap_defsym(<out> <value>)
#   The ld argument defining KICKOS_USER_HEAP_SIZE as <value>, a byte count; anything else would
#   reach ld as a malformed --defsym.
function(kickos_heap_defsym out value)
  if(NOT value MATCHES "^(0x[0-9A-Fa-f]+|[0-9]+)$")
    message(FATAL_ERROR "kickos_heap_defsym: '${value}' is no byte count")
  endif()
  set(${out} "--defsym=KICKOS_USER_HEAP_SIZE=${value}" PARENT_SCOPE)
endfunction()
