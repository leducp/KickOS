# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# libcrace_image(<target> <quantum ns>)
#   main.cc as <target> and peer.cc as <target>_peer, their racers on a round-robin quantum of
#   <quantum ns>.
function(libcrace_image target quantum_ns)
  add_executable(${target} main.cc)
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
  target_compile_definitions(${target} PRIVATE LIBCRACE_QUANTUM_NS=${quantum_ns}u)
  add_executable(${target}_peer peer.cc)
  target_link_libraries(${target}_peer PRIVATE kickos)
  target_compile_definitions(${target}_peer PRIVATE LIBCRACE_QUANTUM_NS=${quantum_ns}u)
  target_compile_definitions(${target} PRIVATE "LIBCRACE_PEER=\"${target}_peer\"")
  set(LIBCRACE_APP ${target})
  set(LIBCRACE_PEER_THREADS 3)
  # The nRF51's 32 KiB arena seats six thread slots and the release arm needs seven: this image
  # alone links against more SRAM, and its emulator runs are given the same.
  if(KICKOS_CHIP STREQUAL "nrf51")
    set(_libcrace_sram 36864)
    target_link_options(${target} PRIVATE
                        "LINKER:--defsym=__kickos_ram_length=${_libcrace_sram}")
    set_target_properties(${target} PROPERTIES
      KICKOS_IMAGE_QEMU_ARGS "-global;nrf51-soc.sram-size=${_libcrace_sram}")
  endif()
  configure_file(system.yaml.in "${CMAKE_CURRENT_BINARY_DIR}/${target}.yaml" @ONLY)
  kickos_app_system(${target} COMPOSITION "${CMAKE_CURRENT_BINARY_DIR}/${target}.yaml")
  kickos_emit_image(${target})
endfunction()

libcrace_image(libcrace 20000)
# The image an emulator runs with an interrupt deliverable between any two instructions.
kickos_qemu_machine("${KICKOS_BOARD}" _libcrace_env _libcrace_machine)
if(NOT _libcrace_machine STREQUAL "")
  libcrace_image(libcrace_any_insn 200000)
endif()
