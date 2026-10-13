# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The AMP partition as the build reads it, at CONFIGURE time. genconfig.py validates the partition
# and derives this image's part of it; what is left here needs the chip's facts or is the build
# graph's.
#
# It must stay include()d at directory scope: the root file and everything under it read the
# variables it sets, and a function scope would not export them.
#
# The tasks' share as C reads it (<kickos/config/amp_ports.h>): zero where the image is no
# own-image node or the partition states none.
set(KICKOS_AMP_USER_SHARE_ADDR 0x0)
set(KICKOS_AMP_USER_SHARE_BYTES 0x0)
set(KICKOS_AMP_USER_SHARE_NC 0)
if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  set(_amp_user_share "${KICKOS_AMP_USER_SHARE_SIZE}")
  include(amp_share)
  kickos_amp_share_check(SHARE "${_amp_user_share}" WINDOW "${KICKOS_AMP_SHARED_SIZE}"
                         UNCACHED "${KICKOS_AMP_USER_SHARE_UNCACHED}"
                         TRANSLATES "${KICKOS_HAVE_ASPACE}" ORIGIN "${KICKOS_DEFCONFIG}")
  math(EXPR KICKOS_AMP_USER_SHARE_ADDR
       "${KICKOS_AMP_PARTITION_BASE} + ${KICKOS_AMP_NODES} * ${KICKOS_AMP_NODE_SHARE} \
        + ${KICKOS_AMP_SHARED_SIZE} - ${_amp_user_share}"
       OUTPUT_FORMAT HEXADECIMAL)
  kickos_amp_share_granule_check(SHARE "${_amp_user_share}" BASE "${KICKOS_AMP_USER_SHARE_ADDR}"
                                 MIN_REGION "${KICKOS_CHIP_REGION_GRANULE}"
                                 POW2 "${KICKOS_CHIP_REGION_POW2}" ORIGIN "${KICKOS_DEFCONFIG}")
  math(EXPR KICKOS_AMP_USER_SHARE_BYTES "${_amp_user_share}" OUTPUT_FORMAT HEXADECIMAL)
  if(KICKOS_AMP_USER_SHARE_UNCACHED)
    set(KICKOS_AMP_USER_SHARE_NC 1)
  endif()

  add_compile_definitions(KICKOS_AMP_NODE_CORE_LIST={${KICKOS_AMP_NODE_CORES}})
  add_compile_definitions(KICKOS_AMP_SELF_CORE=${KICKOS_AMP_SELF_CORE})
  # The geometry reaches a chip's startup ASSEMBLY too: its translation tables map the
  # partition.
  add_compile_definitions(
    KICKOS_AMP_PARTITION_BASE=${KICKOS_AMP_PARTITION_BASE}
    KICKOS_AMP_NODE_SHARE=${KICKOS_AMP_NODE_SHARE}
    KICKOS_AMP_SHARED_SIZE=${KICKOS_AMP_SHARED_SIZE}
    KICKOS_AMP_USER_SHARE_SIZE=${_amp_user_share}
    KICKOS_AMP_USER_SHARE_UNCACHED=${KICKOS_AMP_USER_SHARE_NC}
    KICKOS_AMP_NODE_ID=${KICKOS_AMP_NODE_ID}
    KICKOS_AMP_RESET_OWNER=${KICKOS_AMP_RESET_OWNER}
    KICKOS_AMP_TEXT_BASE=${KICKOS_AMP_TEXT_BASE}
    KICKOS_AMP_TEXT_SHARE=${KICKOS_AMP_TEXT_SHARE})
  # The fingerprint covers the Kconfig half alone: two images differing only in a cache entry
  # that is no Kconfig symbol, KICKOS_APPDATA_SIZE among them, carry the same one.
  # build-partition.sh covers that half by configuring every peer from node 0's cache entries.
  add_link_options("LINKER:--defsym=kickos_amp_config_fp_hi=0x${KICKOS_AMP_CONFIG_FP_HI}"
                   "LINKER:--defsym=kickos_amp_config_fp_lo=0x${KICKOS_AMP_CONFIG_FP_LO}")
  message(STATUS
    "KickOS: AMP node ${KICKOS_AMP_NODE_ID} of ${KICKOS_AMP_NODES} on core "
    "${KICKOS_AMP_SELF_CORE}, map ${KICKOS_AMP_NODE_CORES}, kconfig "
    "${KICKOS_AMP_CONFIG_FP_HI}${KICKOS_AMP_CONFIG_FP_LO}")
endif()

if(KICKOS_AMP_NODE)
  math(EXPR _amp_port_far "${KICKOS_AMP_PORT_COUNT} - ${KICKOS_AMP_PORT_LOCAL}")
  message(STATUS
    "KickOS: AMP node ${KICKOS_AMP_SELF_NODE} derives ${KICKOS_AMP_PORT_LOCAL} local port(s) and "
    "${_amp_port_far} far endpoint(s) from KICKOS_AMP_PORTS='${KICKOS_AMP_PORTS}'")
endif()
