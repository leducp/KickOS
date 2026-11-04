# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The consumer behind the image arms of tests/integration/check_oot_export_mcu.sh: the image
# image_link, the DEFAULT target of its system (system.yaml), runs its own main and the program
# image_link_program, whose main reaches one symbol out of each input whose place in the
# program's link CMake alone computes:
#   kickos_ilk_iface   an INTERFACE source of an INTERFACE target the program links through
#                      another INTERFACE target, which also carries KickOS::kickos;
#   kickos_ilk_leaf    a STATIC library linked PRIVATE by another STATIC library, so it is in
#                      no LINK_LIBRARIES of the program;
#   kickos_ilk_object  an OBJECT library;
#   kickos_ilk_throw   a throw and its catch, so the exception runtime's --wrap rides the link.

cmake_minimum_required(VERSION 3.24)
project(kickos_image_link LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

add_library(ilk_inner INTERFACE)
target_sources(ilk_inner INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/iface.c")
target_link_libraries(ilk_inner INTERFACE KickOS::kickos)
add_library(ilk_outer INTERFACE)
target_link_libraries(ilk_outer INTERFACE ilk_inner)

add_library(ilk_leaf STATIC leaf.c)
add_library(ilk_mid STATIC mid.c)
target_link_libraries(ilk_mid PRIVATE ilk_leaf)

add_library(ilk_object OBJECT object.c)

add_executable(image_link_program main.cc)
target_link_libraries(image_link_program PRIVATE ilk_outer ilk_mid ilk_object)

add_executable(image_link image.cc)
target_link_libraries(image_link PRIVATE KickOS::kickos)
kickos_compose(image_link_system system.yaml DEFAULT image_link)
kickos_emit_image(image_link_system)
