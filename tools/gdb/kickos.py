# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# gdb helper for a KickOS board whose images are programs embedded in the kernel's link (an MMU
# board). Every task in a space of its own runs a program whose symbols are in that program's ELF,
# <kernel ELF>.kimg/image<n>.elf, and not in the kernel's. On x86_64 the kernel ELF is
# <image>.efi.elf, the PE32+ image's ELF twin (tools/x86_64-link.sh), and a program's is
# <image>.efi.kimg/image<n>.elf.
#
#   (gdb) file <kernel ELF>
#   (gdb) source tools/gdb/kickos.py
#
# On every stop it reads the stopped core's current thread, its task, the task's space and the
# image the task runs out of the kernel's symbols, and when the task runs in a space it loads that
# image's program ELF in place of the one loaded before. `kos-space <task>` does the same for
# <task>, an expression naming a kickos::Task; `kos-space` alone repeats the stop's choice.
# Either prints what it chose. `$kos_image()` is the image the stopped core's task runs, -1 for
# none, so a breakpoint at an address every image's space may run can hold to one image:
# `condition <bp> $kos_image() == 2`.
#
# A program is loaded at the address its image header runs at, which the kernel's image table
# holds, less the address its own link gave the header.

import os
import struct

import gdb

# The kernel's one instance: per_instance[0] is at its address.
INSTANCE = 'kickos::detail::g_instance'
HEADER_SECTION = '.kickos_image0_header'


def header_address(path):
    """The address `path`'s own link gave its image header section."""
    with open(path, 'rb') as fh:
        elf = fh.read()
    if elf[:4] != b'\x7fELF' or elf[4] != 2 or elf[5] != 1:
        raise gdb.GdbError('%s: not an ELF64 little-endian file' % path)
    shoff, = struct.unpack_from('<Q', elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', elf, 0x3a)
    names_at = struct.unpack_from('<Q', elf, shoff + shstrndx * shentsize + 0x18)[0]
    for i in range(shnum):
        name, _, _, addr = struct.unpack_from('<IIQQ', elf, shoff + i * shentsize)
        end = elf.index(b'\0', names_at + name)
        if elf[names_at + name:end].decode() == HEADER_SECTION:
            return addr
    raise gdb.GdbError('%s holds no %s section' % (path, HEADER_SECTION))


class Space(object):
    loaded = None


def program_elf(n):
    kernel = gdb.current_progspace().filename
    if not kernel:
        raise gdb.GdbError('kos-space: no kernel ELF loaded (file <kernel ELF>)')
    if kernel.endswith('.efi.elf'):
        kernel = kernel[:-len('.elf')]
    return os.path.join(kernel + '.kimg', 'image%d.elf' % n)


def field_offset(type_name, path):
    """The byte offset of the member `path` (dotted) inside `type_name`."""
    t = gdb.lookup_type(type_name)
    offset = 0
    for name in path.split('.'):
        t = t.strip_typedefs()
        for f in t.fields():
            if f.name == name:
                offset += f.bitpos // 8
                t = f.type
                break
        else:
            raise gdb.GdbError('kos-space: %s has no member %s' % (type_name, path))
    return offset, t


def address_of(name):
    """A kernel symbol's address. By symbol, not by expression: at a stop in a program the
    frame's language may be C, which cannot spell a namespace."""
    symbol = gdb.lookup_global_symbol(name) or gdb.lookup_static_symbol(name)
    if symbol is not None:
        return int(symbol.value().address)
    return int(gdb.parse_and_eval('(unsigned long)&%s' % name))


def word(va, size=8):
    """The kernel's little-endian word of `size` bytes at `va`, a pointer by default. A stop in an
    unprivileged mode may see no kernel page, so a refused read is retried at the frame the
    kernel's high alias names, which QEMU's stub reads in its physical mode."""
    inferior = gdb.selected_inferior()
    try:
        return int.from_bytes(bytes(inferior.read_memory(va, size)), 'little')
    except gdb.MemoryError:
        pa = va - address_of('__kickos_frame_pool_delta')
        gdb.execute('maintenance packet Qqemu.PhyMemMode:1', to_string=True)
        try:
            return int.from_bytes(bytes(inferior.read_memory(pa, size)), 'little')
        finally:
            gdb.execute('maintenance packet Qqemu.PhyMemMode:0', to_string=True)


def current_task():
    """The task of the stopped core's current thread, as an address, or 0."""
    cells, cells_type = field_offset('kickos::Kernel', 'currents.cells')
    count = cells_type.range()[1] + 1
    core = 0
    thread = gdb.selected_thread()
    if thread is not None and count > 1:
        core = min(thread.num - 1, count - 1)
    current = word(address_of(INSTANCE) + cells + 8 * core)
    if current == 0:
        return 0
    return word(current + field_offset('kickos::Thread', 'task')[0])


def program_of(task):
    """The image a task runs, or None for a task in no space of its own."""
    if task == 0:
        return None
    domain = word(task + field_offset('kickos::Task', 'domain')[0])
    if domain == 0 or word(domain + field_offset('kickos::Domain', 'space')[0]) == 0:
        return None
    at, image = field_offset('kickos::Task', 'image')
    return word(task + at, image.strip_typedefs().sizeof)


def offset_of(n, path):
    return word(address_of('__kickos_image_table') + 8 * n) - header_address(path)


def load(n):
    path = None
    if n is not None:
        path = program_elf(n)
        if not os.path.isfile(path):
            raise gdb.GdbError('kos-space: no program ELF at %s' % path)
    if path == Space.loaded:
        return path
    if Space.loaded is not None:
        gdb.execute('remove-symbol-file %s' % Space.loaded, to_string=True)
        Space.loaded = None
    if path is not None:
        gdb.execute('add-symbol-file %s -o %#x' % (path, offset_of(n, path)),
                    to_string=True)
        Space.loaded = path
    return path


def report(task, n, path):
    if path is None:
        print('kos-space: task %#x runs in no space of its own; kernel symbols only' % task)
    else:
        print('kos-space: task %#x runs image %d, %s' % (task, n, path))


class KosSpace(gdb.Command):
    """Load the ELF of the program a task runs: kos-space [<kickos::Task expression>]."""

    def __init__(self):
        super(KosSpace, self).__init__('kos-space', gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        if arg.strip():
            task = gdb.parse_and_eval(arg)
            if task.type.code != gdb.TYPE_CODE_PTR:
                task = task.address
            task = int(task)
        else:
            task = current_task()
        n = program_of(task)
        report(task, n, load(n))


class KosImage(gdb.Function):
    """The image the stopped core's task runs, or -1 for a task in no space of its own."""

    def __init__(self):
        super(KosImage, self).__init__('kos_image')

    def invoke(self):
        n = program_of(current_task())
        if n is None:
            return -1
        return n


def on_stop(_event):
    try:
        load(program_of(current_task()))
    except (gdb.error, gdb.GdbError) as e:
        print('kos-space: %s' % e)


KosSpace()
KosImage()
gdb.events.stop.connect(on_stop)
