# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The RAM region rule: the bytes one region descriptor names for a block and the alignment the
# block sits on, as arch_ram_region_size and arch_ram_region_align answer them
# (arch/include/kickos/arch/arch.h). tests/unit/ramalign holds the two equal.

from .manifest import Manifest

# What a build whose arch_mpu_min_region is 0 rounds and aligns to.
NO_UNIT_GRANULE = 16


def seam_manifest(min_region, pow2, stride):
    """The window rule and stride of a build with these seams."""
    manifest = Manifest()
    # Not the export manifest's granule of 16, which rounds a want of 0 up where C answers 0.
    manifest.window_rule = "none"
    if min_region != 0:
        manifest.window_rule = "granule"
        manifest.smallest_window = min_region
        if pow2 != 0:
            manifest.window_rule = "pow2"
    manifest.stack_stride = None
    if stride != 0:
        manifest.stack_stride = stride
    return manifest


def ram_size(want, manifest):
    """arch_ram_region_size()."""
    if manifest.window_rule not in ("pow2", "granule"):
        return -(-want // NO_UNIT_GRANULE) * NO_UNIT_GRANULE
    smallest = manifest.smallest_window
    want = max(want, smallest)
    if manifest.window_rule == "granule":
        return -(-want // smallest) * smallest
    return pow2_ceil(want)


def ram_align(want, manifest):
    """arch_ram_region_align()."""
    geometry = NO_UNIT_GRANULE
    if manifest.window_rule == "granule":
        geometry = manifest.smallest_window
    if manifest.window_rule == "pow2":
        geometry = ram_size(want, manifest)
    stride = manifest.stack_stride
    if stride is not None and ram_size(want, manifest) == stride and stride > geometry:
        geometry = stride
    return geometry


def pow2_ceil(want):
    power = 1
    while power < want:
        power = power * 2
    return power


def boot_figures(wants, min_region, pow2, stride, word_bits):
    """Each want's (size, alignment) on a build with these seams, or the first want whose figure a
    word of `word_bits` cannot hold, which arch_ram_region_size answers unrounded."""
    manifest = seam_manifest(min_region, pow2, stride)
    figures = []
    for want in wants:
        size, align = ram_size(want, manifest), ram_align(want, manifest)
        if max(size, align) >> word_bits:
            return None, want
        figures.append((size, align))
    return figures, None
