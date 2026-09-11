# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The AMP partition's geometry and its port capabilities, resolved at CONFIGURE time.
#
# ORDERING IS LOAD-BEARING: this runs before cap_table.cmake, whose width has a term per
# listed crossing (KICKOS_AMP_PORT_COUNT below). It must stay include()d at directory scope:
# the root file and everything under it read the variables it sets, and a function scope
# would not export them.
# --- The AMP partition's geometry -------------------------------------------------------
# arch/common/amp_partition.ld.h computes every address from a node index and these three
# facts.
if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  foreach(_geo KICKOS_AMP_PARTITION_BASE KICKOS_AMP_NODE_SHARE KICKOS_AMP_SHARED_SIZE)
    # math() normalises every spelling of zero; a string test would pass 0x00.
    set(_geo_v 0)
    if(NOT "${${_geo}}" STREQUAL "")
      math(EXPR _geo_v "${${_geo}}")
    endif()
    if(_geo_v EQUAL 0)
      message(FATAL_ERROR
        "KickOS: the own-image AMP posture states no ${_geo}. Every node's image is linked at "
        "KICKOS_AMP_PARTITION_BASE plus its own index times KICKOS_AMP_NODE_SHARE, and the "
        "region every node writes sits above the last of them and is KICKOS_AMP_SHARED_SIZE "
        "wide. With any of the three unstated the images link on top of each other. Give all "
        "three in boards/<board>/configs/<variant>/defconfig.")
    endif()
  endforeach()

  # The second span is a pair or it is nothing. Half stated otherwise fails in the link, and
  # obscurely: ld reports a FLASH overflow that names neither knob.
  set(_amp_text_v "")
  foreach(_geo KICKOS_AMP_TEXT_BASE KICKOS_AMP_TEXT_SHARE)
    set(_geo_v 0)
    if(NOT "${${_geo}}" STREQUAL "")
      math(EXPR _geo_v "${${_geo}}")
    endif()
    list(APPEND _amp_text_v "${_geo_v}")
  endforeach()
  list(GET _amp_text_v 0 _amp_text_base_v)
  list(GET _amp_text_v 1 _amp_text_share_v)
  if(_amp_text_base_v EQUAL 0 AND NOT _amp_text_share_v EQUAL 0)
    message(FATAL_ERROR
      "KickOS: the own-image AMP posture states KICKOS_AMP_TEXT_SHARE and no "
      "KICKOS_AMP_TEXT_BASE. The two describe ONE aperture, the separate span a part executing "
      "out of execute-in-place memory keeps its text in, and a node's text is linked at that "
      "base plus its own index times that stride. A stride without a base puts every node's "
      "text at zero. State both, or neither, in boards/<board>/configs/<variant>/defconfig: "
      "neither means the part has one span and KICKOS_AMP_PARTITION_BASE describes all of it.")
  endif()
  if(_amp_text_share_v EQUAL 0 AND NOT _amp_text_base_v EQUAL 0)
    message(FATAL_ERROR
      "KickOS: the own-image AMP posture states KICKOS_AMP_TEXT_BASE and no "
      "KICKOS_AMP_TEXT_SHARE. The two describe ONE aperture, the separate span a part executing "
      "out of execute-in-place memory keeps its text in, and a node's text is linked at that "
      "base plus its own index times that stride. A base without a stride links every node's "
      "text at the same address and gives the region no length. State both, or neither, in "
      "boards/<board>/configs/<variant>/defconfig: neither means the part has one span and "
      "KICKOS_AMP_PARTITION_BASE describes all of it.")
  endif()

  string(REPLACE "," ";" _amp_cores "${KICKOS_AMP_NODE_CORES}")
  list(LENGTH _amp_cores _amp_cores_len)
  if(NOT _amp_cores_len EQUAL KICKOS_AMP_NODES)
    message(FATAL_ERROR
      "KickOS: KICKOS_AMP_NODE_CORES is '${KICKOS_AMP_NODE_CORES}', which names "
      "${_amp_cores_len} cores for a partition of ${KICKOS_AMP_NODES} nodes. It states, per "
      "node and in node order, which machine core carries that node, and amp::ring takes its "
      "doorbell target from it. Without one a node index would be spent as a hardware core "
      "mask, which holds only under CONFIG_KICKOS_AMP_POSTURE_SHARED_IMAGE. Give one core per "
      "node in boards/<board>/configs/<variant>/defconfig.")
  endif()
  set(_amp_seen "")
  set(_amp_node 0)
  foreach(_core IN LISTS _amp_cores)
    if(NOT _core MATCHES "^[0-9]+$")
      message(FATAL_ERROR
        "KickOS: KICKOS_AMP_NODE_CORES entry '${_core}' is not a core index.")
    endif()
    if(_core GREATER_EQUAL KICKOS_AMP_PARTITION_CORES)
      message(FATAL_ERROR
        "KickOS: KICKOS_AMP_NODE_CORES puts node ${_amp_node} on core ${_core}, and the "
        "partition spans ${KICKOS_AMP_PARTITION_CORES} cores. The doorbell's cells are indexed "
        "by core, so this node would answer out of the matrix.")
    endif()
    if(_core IN_LIST _amp_seen)
      message(FATAL_ERROR
        "KickOS: KICKOS_AMP_NODE_CORES puts two nodes on core ${_core}. Each node's image "
        "drives its own core, and the doorbell's rendezvous row is written by that core alone.")
    endif()
    list(APPEND _amp_seen "${_core}")
    math(EXPR _amp_node "${_amp_node} + 1")
  endforeach()
  list(GET _amp_cores 0 _amp_node0_core)
  if(NOT _amp_node0_core EQUAL 0)
    message(FATAL_ERROR
      "KickOS: KICKOS_AMP_NODE_CORES is '${KICKOS_AMP_NODE_CORES}', which puts node 0 on core "
      "${_amp_node0_core}. Node 0 is the partition primary and runs on the core the machine "
      "resets into, which this tree takes to be core 0. Stated otherwise, some peer is mapped "
      "onto the core the primary is already running on, and releasing that peer is the primary "
      "releasing itself: the boot dies in the launch handshake rather than at build time, on "
      "qemu-arm64 as 'KickOS: PSCI CPU_ON refused for AMP node 01, status "
      "0xfffffffffffffffc', which is PSCI_ALREADY_ON. Give node 0 core 0 and permute the peers "
      "instead.")
  endif()

  list(GET _amp_cores ${KICKOS_AMP_NODE_ID} KICKOS_AMP_SELF_CORE)
  string(REPLACE ";" "," _amp_core_init "${_amp_cores}")
  add_compile_definitions(KICKOS_AMP_NODE_CORE_LIST={${_amp_core_init}})
  add_compile_definitions(KICKOS_AMP_SELF_CORE=${KICKOS_AMP_SELF_CORE})
  # The geometry reaches a chip's startup ASSEMBLY too: its translation tables map the
  # partition.
  add_compile_definitions(
    KICKOS_AMP_PARTITION_BASE=${KICKOS_AMP_PARTITION_BASE}
    KICKOS_AMP_NODE_SHARE=${KICKOS_AMP_NODE_SHARE}
    KICKOS_AMP_SHARED_SIZE=${KICKOS_AMP_SHARED_SIZE}
    KICKOS_AMP_NODE_ID=${KICKOS_AMP_NODE_ID}
    KICKOS_AMP_TEXT_BASE=${KICKOS_AMP_TEXT_BASE}
    KICKOS_AMP_TEXT_SHARE=${KICKOS_AMP_TEXT_SHARE})
  # --- What a peer node is configured from, and what proves two images agree ---------------
  # The seed is this node's RESOLVED .config, which is half of what a peer is configured from:
  # build-partition.sh sweeps the cache beside it for the knobs that are no Kconfig symbol.
  #
  # KICKOS_AMP_NODE_ID is the one symbol a node may differ in: the seed omits it and
  # build-partition.sh states it per node.
  file(STRINGS "${PROJECT_BINARY_DIR}/generated/.config" _amp_cfg_lines
       REGEX "^CONFIG_[A-Z0-9_]+=")
  foreach(_line IN LISTS _amp_cfg_lines)
    string(REGEX REPLACE "^CONFIG_([A-Z0-9_]+)=.*$" "\\1" _amp_n "${_line}")
    string(REGEX REPLACE "^CONFIG_[A-Z0-9_]+=" "" _amp_v "${_line}")
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" _amp_cfg_${_amp_n} "${_amp_v}")
  endforeach()

  # An initial-cache script: a seeded value carrying a space or a semicolon reaches the peer
  # whole.
  set(_amp_seed "# Written by node ${KICKOS_AMP_NODE_ID}'s configure. Read by tools/amp/build-partition.sh.\n")
  foreach(_amp_name IN LISTS KICKOS_KCONFIG_PROMPTED_VALUE
                    ITEMS KICKOS_CONSOLE KICKOS_TELEMETRY)
    if(_amp_name STREQUAL "KICKOS_AMP_NODE_ID")
      continue()
    endif()
    if(DEFINED _amp_cfg_${_amp_name})
      string(APPEND _amp_seed
             "set(${_amp_name} \"${_amp_cfg_${_amp_name}}\" CACHE STRING \"\" FORCE)\n")
    endif()
  endforeach()
  # Both directions, and unconditionally: kconfiglib writes an off bool as a COMMENT, not as
  # `=n`, so absent from the resolved .config is off and not unstated. Seeded as unstated the
  # peer would take its Kconfig default.
  foreach(_amp_name IN LISTS KICKOS_KCONFIG_PROMPTED_FLAG)
    set(_amp_v "OFF")
    if(DEFINED _amp_cfg_${_amp_name} AND _amp_cfg_${_amp_name} STREQUAL "y")
      set(_amp_v "ON")
    endif()
    string(APPEND _amp_seed "set(${_amp_name} ${_amp_v} CACHE BOOL \"\" FORCE)\n")
  endforeach()
  file(WRITE "${PROJECT_BINARY_DIR}/generated/amp-peer-seed.cmake" "${_amp_seed}")

  # The fingerprint the IMAGE carries, over that same resolved configuration with the one
  # per-node symbol taken out. TWO absolute symbols of 32 bits: a 32-bit linker truncates one
  # 64-bit --defsym without saying so.
  #
  # IT COVERS THE KCONFIG HALF ALONE, which is narrower than "two nodes agree": two images
  # differing only in an input that is a CMake cache entry and no Kconfig symbol carry the SAME
  # fingerprint, KICKOS_APPDATA_SIZE=112K and =120K among them, and so do the toolchain and the
  # build type. build-partition.sh covers that half, configuring every peer FROM node 0's own
  # KICKOS_* cache entries. The hash cannot be widened here: the cache is still incomplete at
  # this point, KICKOS_INIT_PROVIDER being declared further down this file.
  string(REGEX REPLACE "CONFIG_KICKOS_AMP_NODE_ID=[0-9]+" "" _amp_common "${_amp_cfg_lines}")
  string(SHA256 _amp_fp "${_amp_common}")
  string(SUBSTRING "${_amp_fp}" 0 8 _amp_fp_hi)
  string(SUBSTRING "${_amp_fp}" 8 8 _amp_fp_lo)
  add_link_options("LINKER:--defsym=kickos_amp_config_fp_hi=0x${_amp_fp_hi}"
                   "LINKER:--defsym=kickos_amp_config_fp_lo=0x${_amp_fp_lo}")

  message(STATUS
    "KickOS: AMP node ${KICKOS_AMP_NODE_ID} of ${KICKOS_AMP_NODES} on core "
    "${KICKOS_AMP_SELF_CORE}, map ${KICKOS_AMP_NODE_CORES}, kconfig "
    "${_amp_fp_hi}${_amp_fp_lo}")
endif()

# --- The partition's port capabilities ---------------------------------------------------
# ONE list for the whole partition: every node's image reads the same string and derives both
# its sets from its own index.
#
# The order here is the order the kernel seats them into root's table, so an entry's position
# IS its capability index.
if(KICKOS_AMP_NODE)
  # Which node this image seats for. Under the shared image the identity is a core register
  # and exactly one core runs a kernel, the one kmain boots on; kernel/init/kmain.cc asserts
  # the two agree.
  set(KICKOS_AMP_SELF_NODE 0)
  if(KICKOS_AMP_OWN_IMAGE)
    set(KICKOS_AMP_SELF_NODE "${KICKOS_AMP_NODE_ID}")
  endif()

  string(REPLACE "," ";" _amp_ports "${KICKOS_AMP_PORTS}")
  list(LENGTH _amp_ports _amp_ports_len)
  if("${KICKOS_AMP_PORTS}" STREQUAL "")
    set(_amp_ports "")
    set(_amp_ports_len 0)
  endif()
  if(_amp_ports_len EQUAL 0)
    message(FATAL_ERROR
      "KickOS: this board is a node of an AMP partition and KICKOS_AMP_PORTS names no "
      "crossing at all. A node whose partition hands it no port capability can neither be "
      "called nor call, and it would boot and answer nothing with nothing to say so. State "
      "the whole partition's ports once, as node:port pairs in node order, in "
      "boards/<board>/configs/<variant>/defconfig. For example "
      "CONFIG_KICKOS_AMP_PORTS=\"0:2,1:3\" for a node 0 serving port 2 and a node 1 "
      "serving port 3.")
  endif()

  set(_amp_port_nodes "")
  set(_amp_port_ports "")
  set(_amp_port_seen "")
  set(_amp_port_local 0)
  foreach(_entry IN LISTS _amp_ports)
    if(NOT _entry MATCHES "^([0-9]+):([0-9]+)$")
      message(FATAL_ERROR
        "KickOS: KICKOS_AMP_PORTS entry '${_entry}' is not a node:port pair. The list names, "
        "for the WHOLE partition, which node serves which port; every node's image reads it "
        "and derives its own two sets from its own index.")
    endif()
    set(_n "${CMAKE_MATCH_1}")
    set(_p "${CMAKE_MATCH_2}")
    if(_n GREATER_EQUAL KICKOS_AMP_NODES)
      message(FATAL_ERROR
        "KickOS: KICKOS_AMP_PORTS entry '${_entry}' names node ${_n} and the partition holds "
        "${KICKOS_AMP_NODES} node(s). A crossing to a node the partition does not hold has "
        "no ring to travel on.")
    endif()
    if(_p EQUAL 1)
      message(FATAL_ERROR
        "KickOS: KICKOS_AMP_PORTS entry '${_entry}' names port 1, which is the port every "
        "reply travels on: a message published on it is a REPLY and is routed to the caller "
        "its token names, so no node can serve it. Service ports start at 2.")
    endif()
    if(_p EQUAL 0 AND _n EQUAL KICKOS_AMP_SELF_NODE)
      message(FATAL_ERROR
        "KickOS: KICKOS_AMP_PORTS entry '${_entry}' names port 0 for this image's own node. "
        "Port 0 is the echo the WINDOW LAYER answers with no thread involved, and a node that "
        "runs a kernel binds every port the partition names it, so this entry would take the "
        "echo away from the window layer and hand it to a thread. Name port 0 only for a peer "
        "that runs no kernel of its own, or give this node a service port from 2 up.")
    endif()
    if(_p GREATER_EQUAL 32)
      message(FATAL_ERROR
        "KickOS: KICKOS_AMP_PORTS entry '${_entry}' names port ${_p} and a node's mint is a "
        "32-bit mask, so 31 is its last port.")
    endif()
    if("${_n}:${_p}" IN_LIST _amp_port_seen)
      message(FATAL_ERROR
        "KickOS: KICKOS_AMP_PORTS names node ${_n} port ${_p} twice. A repeated crossing "
        "would seat two capabilities for one endpoint and shift every constant after it.")
    endif()
    list(APPEND _amp_port_seen "${_n}:${_p}")
    list(APPEND _amp_port_nodes "${_n}")
    list(APPEND _amp_port_ports "${_p}")
    if(_n EQUAL KICKOS_AMP_SELF_NODE)
      math(EXPR _amp_port_local "${_amp_port_local} + 1")
    endif()
  endforeach()

  if(NOT KICKOS_MAX_ENDPOINTS GREATER _amp_ports_len)
    message(FATAL_ERROR
      "KickOS: KICKOS_AMP_PORTS names ${_amp_ports_len} crossing(s) and the board provisions "
      "${KICKOS_MAX_ENDPOINTS} endpoint slot(s). Each entry claims one for the life of the "
      "image, a local one for the port it binds and a far one for the crossing it names, so "
      "this partition cannot seat itself and would leave nothing for the app besides. Raise "
      "CONFIG_KICKOS_MAX_ENDPOINTS in this board's defconfig.")
  endif()

  # The ports are only ONE of the seats root holds against KICKOS_TASK_ENDPOINT_BUDGET; the
  # rest are a service list's, whose target does not exist this early. That relation is
  # kickos_endpoint_seats_check (cmake/cap_table.cmake).

  set(KICKOS_AMP_PORT_COUNT "${_amp_ports_len}")
  string(REPLACE ";" "," KICKOS_AMP_PORT_NODE_INIT "${_amp_port_nodes}")
  string(REPLACE ";" "," KICKOS_AMP_PORT_PORT_INIT "${_amp_port_ports}")
  math(EXPR _amp_port_far "${KICKOS_AMP_PORT_COUNT} - ${_amp_port_local}")
  message(STATUS
    "KickOS: AMP node ${KICKOS_AMP_SELF_NODE} derives ${_amp_port_local} local port(s) and "
    "${_amp_port_far} far endpoint(s) from KICKOS_AMP_PORTS='${KICKOS_AMP_PORTS}'")
else()
  set(KICKOS_AMP_SELF_NODE 0)
  set(KICKOS_AMP_PORT_COUNT 0)
  set(KICKOS_AMP_PORT_NODE_INIT "")
  set(KICKOS_AMP_PORT_PORT_INIT "")
endif()
