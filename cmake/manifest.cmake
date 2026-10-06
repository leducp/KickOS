# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The export manifest, written at configure by tools/manifest/genmanifest.py. Every value
# Kconfig resolves it reads from .config; this file hands it, as JSON, only what the build graph
# alone knows. Its schema is the host tool's (tools/compose/kickos_compose/manifest.py).

# The layout version of the table a composition is emitted as. Declared on this side, which
# exports it; the table's C header takes it from here, through the generated
# <kickos/sys/table_version.h>.
set(KICKOS_TABLE_VERSION 3)

# Writes <export_dir>/manifest.yaml, and copies the board's chip and board files to
# <export_dir>/platform/<chip>/ and its default composition to <export_dir>/boards/<board>/, so
# the manifest names them by the same relative path in the build tree and in the installed
# package.
function(kickos_export_manifest export_dir)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  _kickos_json_quote("${PROJECT_BINARY_DIR}/generated/.config" _config)

  # The two seams as the boot-arena model scraped them, or scraped here where no linker script
  # took that path. Left out on the sim, whose granule genmanifest.py reads from the host.
  get_property(_mn GLOBAL PROPERTY KICKOS_MPU_MIN_REGION_CFG)
  get_property(_p2 GLOBAL PROPERTY KICKOS_MPU_REGION_POW2_CFG)
  if("${_mn}" STREQUAL "" AND TARGET kickos_chip_${KICKOS_CHIP} AND TARGET kickos_arch_${KICKOS_ARCH})
    include("${PROJECT_SOURCE_DIR}/cmake/boot_arena.cmake")
    _kickos_target_cc_sources(kickos_arch_${KICKOS_ARCH} "${PROJECT_SOURCE_DIR}/arch" _arch_srcs)
    _kickos_target_cc_sources(kickos_chip_${KICKOS_CHIP} "${PROJECT_SOURCE_DIR}/arch" _chip_srcs)
    _kickos_seam_int("${_arch_srcs}" "${_chip_srcs}" "arch_mpu_min_region" _mn)
    _kickos_seam_int("${_arch_srcs}" "${_chip_srcs}" "arch_mpu_region_pow2" _p2)
  endif()
  set(_seams "{}")
  if(NOT "${_mn}" STREQUAL "" AND NOT "${_p2}" STREQUAL "")
    get_property(_g GLOBAL PROPERTY KICKOS_NO_UNIT_GRANULE)
    set(_seams "{\"min_region\": ${_mn}, \"region_pow2\": ${_p2}, \"no_unit_granule\": ${_g}}")
  endif()

  get_property(_node GLOBAL PROPERTY KICKOS_AMP_PARTITION_NODE)
  get_property(_amp_ports GLOBAL PROPERTY KICKOS_AMP_PARTITION_PORTS)
  set(_amp "null")
  if(NOT "${_node}" STREQUAL "")
    set(_pairs "")
    foreach(_entry IN LISTS _amp_ports)
      string(REPLACE ":" ", " _pair "${_entry}")
      list(APPEND _pairs "[${_pair}]")
    endforeach()
    _kickos_json_array(_jports ${_pairs})
    set(_amp "{\"node\": ${_node}, \"ports\": ${_jports}}")
  endif()

  file(REMOVE_RECURSE "${export_dir}/platform")
  set(_platform "${PROJECT_SOURCE_DIR}/platform/${KICKOS_CHIP}")
  foreach(_file chip.yaml "${KICKOS_BOARD}.yaml")
    configure_file("${_platform}/${_file}" "${export_dir}/platform/${KICKOS_CHIP}/${_file}" COPYONLY)
  endforeach()
  _kickos_json_quote("platform/${KICKOS_CHIP}/chip.yaml" _qchip)
  _kickos_json_quote("platform/${KICKOS_CHIP}/${KICKOS_BOARD}.yaml" _qboard)
  set(_descriptions "{\"chip\": ${_qchip}, \"board\": ${_qboard}}")

  file(REMOVE_RECURSE "${export_dir}/boards")
  set(_composition "boards/${KICKOS_BOARD}/composition.yaml")
  configure_file("${PROJECT_SOURCE_DIR}/${_composition}" "${export_dir}/${_composition}" COPYONLY)
  _kickos_json_quote("${_composition}" _default)

  set(_drivers "")
  get_property(_catalogue GLOBAL PROPERTY KICKOS_DRIVER_CATALOGUE)
  foreach(_name IN LISTS _catalogue)
    get_target_property(_entry kickos_${_name} KICKOS_DRIVER_CATALOGUE_ENTRY)
    kickos_driver_clients_exist(${_name} "${_entry}")
    _kickos_json_quote("${_name}" _qname)
    list(APPEND _drivers "${_qname}: ${_entry}")
  endforeach()
  list(JOIN _drivers ", " _drivers)

  # Root's region set holds KICKOS_MPU_MAX_REGIONS (cmake/mpu_geometry.cmake). Its static regions
  # and its stack are seated at boot, its domain bringing none, and the init self-grants into the
  # rest.
  if(NOT KICKOS_MPU_MAX_REGIONS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "KickOS: no KICKOS_MPU_MAX_REGIONS; cmake/mpu_geometry.cmake declares it")
  endif()
  get_property(_root_statics GLOBAL PROPERTY KICKOS_ROOT_STATIC_REGIONS)
  if("${_root_statics}" STREQUAL "")
    set(_root_statics 0)
  endif()
  math(EXPR _free_regions "${KICKOS_MPU_MAX_REGIONS} - ${_root_statics} - 1")

  set(_stride "\"none\"")
  if(NOT "${KICKOS_STACK_STRIDE}" STREQUAL "")
    set(_stride ${KICKOS_STACK_STRIDE})
  endif()
  set(_facts "{\"config\": ${_config}, \"abi\": {\"table\": ${KICKOS_TABLE_VERSION}, \"cap_reserved\": ${KICKOS_CAP_FIRST_DYNAMIC}, \"symbol_prefix\": \"${KICKOS_C_SYMBOL_PREFIX}\"}, \
\"seams\": ${_seams}, \"amp\": ${_amp}, \"fault_isolation\": ${KICKOS_FAULT_ISOLATION}, \
\"priority\": [${KICKOS_PRIO_MIN}, ${KICKOS_PRIO_MAX}], \"stack_align\": ${KICKOS_STACK_ALIGN}, \"stack_stride\": ${_stride}, \
\"init\": {\"status_record_size\": ${KICKOS_INIT_STATUS_RECORD_SIZE}, \
\"private_record_size\": ${KICKOS_INIT_PRIVATE_RECORD_SIZE}, \"free_regions\": ${_free_regions}}, \
\"descriptions\": ${_descriptions}, \"default\": ${_default}, \"drivers\": {${_drivers}}}")
  string(JSON _kind ERROR_VARIABLE _bad TYPE "${_facts}")
  if(_bad)
    message(FATAL_ERROR "KickOS: the manifest's build facts are not JSON: ${_bad}\n${_facts}")
  endif()
  kickos_write_if_changed("${export_dir}/manifest.json" "${_facts}\n")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/manifest/genmanifest.py"
            "${export_dir}/manifest.json" "${export_dir}/manifest.yaml"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "KickOS: the export manifest was not written:\n${_err}")
  endif()
  set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${PROJECT_SOURCE_DIR}/tools/manifest/genmanifest.py"
               "${PROJECT_SOURCE_DIR}/tools/compose/kickos_compose/manifest_fields.py")
endfunction()
