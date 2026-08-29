# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trace-clock capability declaration. 0 here because this port ships no arch_trace_now;
# it must agree with HAS_TRACE_CLOCK in arch/Kconfig or the top CMakeLists refuses.
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 0)
endif()
