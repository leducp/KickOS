# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The two checks of the sim's localized images on the host C++ runtime. Each instance is a
# program linked by the sim's image link alone, as an image is; a host program links two.
kickos_image_host_launcher(_imagecxx_launcher --partial)

function(imagecxx_instance name source)
  add_executable(${name} ${source})
  target_compile_options(${name} PRIVATE ${KICKOS_WARN_FLAGS})
  set_target_properties(${name} PROPERTIES SUFFIX ".o"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    CXX_LINKER_LAUNCHER "${_imagecxx_launcher}")
  set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS
    "${KICKOS_IMAGE_LINK_TOOL}" "${KICKOS_IMAGE_NAMES}")
endfunction()

function(imagecxx_host name source)
  set(_objects "")
  foreach(_instance IN LISTS ARGN)
    list(APPEND _objects "${CMAKE_CURRENT_BINARY_DIR}/${_instance}.o")
  endforeach()
  set_source_files_properties(${_objects} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_executable(${name} ${source} ${_objects})
  target_compile_options(${name} PRIVATE ${KICKOS_WARN_FLAGS})
  add_dependencies(${name} ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 30 LABELS host)
endfunction()

imagecxx_instance(imagecxx_throw throw.cc)
imagecxx_instance(imagecxx_catch catch.cc)
imagecxx_instance(imagecxx_exit_a exit_instance.cc)
imagecxx_instance(imagecxx_exit_b exit_instance.cc)
imagecxx_host(image_cxx_exception exception.cc imagecxx_throw imagecxx_catch)
imagecxx_host(image_cxx_atexit atexit.cc imagecxx_exit_a imagecxx_exit_b)
