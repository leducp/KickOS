#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Build an EFI system partition holding one UEFI application as the removable-media
# fallback path firmware boots without an entry in its boot order.
#
#   tools/esp-x86_64.sh <application.efi> <esp.img>
#
# The image is recreated from scratch on every call: a stale BOOTX64.EFI left in a reused
# image boots instead of the one just built, and prints the same banner.
#
# POSIX sh (dash-clean).

set -u

fail() { echo "FAIL: $*" >&2; exit 1; }

if [ "$#" -ne 2 ]; then
    fail "usage: esp-x86_64.sh <application.efi> <esp.img>"
fi
APP="$1"
IMG="$2"

[ -f "$APP" ] || fail "no application at $APP"
for t in dd mformat mmd mcopy; do
    command -v "$t" >/dev/null 2>&1 || fail "$t is not installed (Debian: mtools, coreutils)"
done

# FAT32 needs the cluster count a 48 MiB volume gives; mformat picks FAT16 below that and
# some firmware refuses it as an ESP.
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=48 conv=sparse status=none \
    || fail "dd could not create $IMG"
mformat -i "$IMG" -F -v KICKOSX1 :: || fail "mformat failed on $IMG"
mmd -i "$IMG" ::/EFI || fail "mmd ::/EFI failed"
mmd -i "$IMG" ::/EFI/BOOT || fail "mmd ::/EFI/BOOT failed"
mcopy -i "$IMG" "$APP" ::/EFI/BOOT/BOOTX64.EFI || fail "mcopy of $APP failed"

# A copy that silently did not land leaves the run booting whatever else the volume holds.
mdir -i "$IMG" ::/EFI/BOOT 2>/dev/null | grep -q '^BOOTX64  EFI' \
    || fail "BOOTX64.EFI is not in $IMG after mcopy"

echo "esp: $IMG holds ::/EFI/BOOT/BOOTX64.EFI from $APP"
