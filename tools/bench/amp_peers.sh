#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Sourced by bench.sh, never executed.

# amp_peer_text <kickos_config.cmake>: `<from>:<to>` of each other node's flash text window, where
# the build is an own-image AMP node whose part keeps every node's text in one flash; nothing
# otherwise. Returns 1, printing why, when the config states no AMP posture to read.
amp_peer_text() {
  local cfg=$1 own nodes id base share node from
  [ -f "$cfg" ] || { echo "no $cfg to read the AMP posture from"; return 1; }
  own=$(sed -n 's/^set(KICKOS_AMP_OWN_IMAGE \([0-9]*\))$/\1/p' "$cfg")
  [ -n "$own" ] || { echo "$cfg states no KICKOS_AMP_OWN_IMAGE"; return 1; }
  [ "$own" = 1 ] || return 0
  nodes=$(sed -n 's/^set(KICKOS_AMP_NODES \([0-9]*\))$/\1/p' "$cfg")
  id=$(sed -n 's/^set(KICKOS_AMP_NODE_ID \([0-9]*\))$/\1/p' "$cfg")
  base=$(sed -n 's/^set(KICKOS_AMP_TEXT_BASE \([0-9]*\))$/\1/p' "$cfg")
  share=$(sed -n 's/^set(KICKOS_AMP_TEXT_SHARE \([0-9]*\))$/\1/p' "$cfg")
  if [ -z "$nodes" ] || [ -z "$id" ] || [ -z "$base" ] || [ -z "$share" ]; then
    echo "$cfg states an own-image AMP node without its node count, its id and its text windows"
    return 1
  fi
  [ "$share" != 0 ] || return 0
  for ((node = 0; node < nodes; node++)); do
    [ "$node" -ne "$id" ] || continue
    from=$((base + node * share))
    printf '0x%x:0x%x\n' "$from" "$((from + share))"
  done
}
