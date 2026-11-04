/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The script of an image's own relocatable link (tools/image-link.py), standing in for the
 * executable script a compiler driver's specs add when a link names none. Every input section
 * keeps its name for the final link to place, which places an image's .tbss right after its
 * .tdata, as one span of the link's one thread-local segment.
 */
SECTIONS
{
    .tdata : { *(.tdata .tdata.* .gnu.linkonce.td.*) }
    .tbss : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) }
}
