# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Word-wise memcpy/memset/memmove gate for lib/libc/string.cc. Host-only of necessity:
# the compiler calls these implicitly, so a target image cannot exercise a chosen
# alignment pair or observe an overrun, and the sim build defines them away in favour of
# the host libc. The shim compiles the real source under prefixed names.
kickos_add_unit_test(NAME kstring_words
  SOURCES kstring_shim.cc kstring_words.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/lib/include")

# x86 completes an unaligned word access, so comparing bytes can NEVER witness the
# no-unaligned-access rule the targets depend on. UBSan's alignment check is what turns
# it into a property this gate can fail on. Optional: a host toolchain without it still
# runs every data check, and the target builds are unaffected either way.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=alignment -fno-sanitize-recover=alignment")
check_cxx_source_compiles("int main(){return 0;}" KICKOS_HAVE_UBSAN_ALIGNMENT)
unset(CMAKE_REQUIRED_FLAGS)
if(KICKOS_HAVE_UBSAN_ALIGNMENT)
  target_compile_options(kstring_words PRIVATE -fsanitize=alignment -fno-sanitize-recover=alignment)
  target_link_options(kstring_words PRIVATE -fsanitize=alignment -fno-sanitize-recover=alignment)
else()
  message(STATUS "KickOS: no UBSan alignment check; kstring_words tests data only")
endif()
