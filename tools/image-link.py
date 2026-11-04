# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The link of a KickOS image, run as the target's linker launcher (kickos_image_link in
# cmake/kickos.cmake):
#
#   python3 image-link.py [--embed] <objcopy> <nm> <names file> <kernel objects file>
#                         [<kernel entry>...] --link <command...>
#   python3 image-link.py --host [--partial] <objcopy> <nm> <names file> [<kernel entry>...]
#                         --link <command...>
#   python3 image-link.py --program [--host] <names file> --link <command...>
#
# The link command is CMake's own, whole. Every input outside the kernel's link group is an image's:
# the init's, which an object marks (KOS_IMAGE_INIT_OBJECT, <kickos/sys/image.h>), and one image per
# declared task beside it. The init's objects are image 0's, an object marked
# KOS_IMAGE_TASK_OBJECT(k) is image k + 1's, the objects marked KOS_IMAGE_BOTH_OBJECT, every archive
# and every flag are every image's, and every other object is every task image's. Each image is
# linked on its own first with -r, its references to the bounds the final link states renamed to its
# own (<names file>, `<name> <field>` per line, to __kickos_image<n>_<field>) and every symbol it
# defines made local but its header, image n's renamed kickos_image<n>_header past image 0. The
# final link then takes the command's kernel group, its linker scripts and the images' objects, and
# writes the command's output. No reference an image leaves undefined may resolve there to a
# definition of the kernel's or of another image's, except those bounds and a <kernel entry> the
# arch calls instead of trapping; one --gc-sections drops, and none the link cannot resolve, is
# refused by the link itself.
# <kernel objects file> lists, one per line, the objects the final link takes for the kernel
# beside its group: its copy of what the images link too.
#
# The chip script INCLUDEs what this writes beside the output for every image, from the image
# template the command names as -Wl,--kickos-image-template=<template>
# (arch/<family>/common/image.ld or <chip>.image.ld beside the chip script, run through cpp): their
# code spans (kickos_images_code.ld), their windows (kickos_images.ld, or where the template has a
# window part what precedes them there and the windows in kickos_images_windows.ld), their
# asserts, the thread-local spans of every task image but image 1 (kickos_images_tdata.ld) and
# the image table's entries past image 0's. Where the protection unit takes a power-of-two region
# aligned to its size, a first final link measures each window and the second places them largest
# first, each aligned to its own size.
#
# The partial links' script is image_partial.lds, beside <names file>. Kept in <out>.kimg, the
# one folder of every per-image file: kickos_image<n>.o, and its map image<n>.map.
#
# --host links a sim app into a host executable, its images split as above. The partial link
# takes no host library: the final link takes each once for the process. An image keeps no COMDAT
# group, so nothing it defines merges with another input's. Its `main` becomes <name>_main, <name>
# the output's file stem, past image 1 <name>_main<n>, and its header __kickos_image<n>_header,
# the two names it keeps global;
# a generated table names each header. --partial writes one image's object as the command's
# output and links nothing else.
#
# With --embed, every image of the partition above is a program: its own full link, with the
# script the command names as -Wl,--kickos-image-program=<script>, writes <out>.kimg/image<n>.elf
# and <out>.kimg/image<n>.map, leaving no reference undefined but a weak one no kernel symbol answers; a
# task image's link is rooted at its header, the script's entry being root's, and defines
# __kickos_task_program; each program's link defines __kickos_image<k>_tls_carve for every task
# image k, its own carve where k is the program and 0 elsewhere. Image 0's loadable
# bytes then become <out>.kimg/kickos_image0.o, an .incbin per load segment, placed where the
# program was linked by <out>.kimg/kickos_image0.ld, which the kernel's script includes beside the
# kernel's own bounds taken from the programs (<embed names>, embed_names.txt beside <names
# file>), and an assert per kernel layout word each program states (kickos_image0_checks.ld).
# Every later image n becomes <out>.kimg/kickos_image<n>.o: its text pages then its window's, the
# loader's bytes and zeros to the window's end, in section .kickos_images at
# __kickos_image<n>_store, which the kernel's script places among the kernel's pages. The kernel's
# script includes kickos_image_table.ld, every image's header address, and kickos_image_frames.ld,
# per image its code span, window and thread-local segment and the delta taking an address of
# each span to its frame, past the address of __kickos_image_frames_anchor. The kernel's link is
# the command's kernel group, its symbol definitions and its -T script, and writes the command's
# output.
#
# x86_64's command is tools/x86_64-link.sh's, whose kernel link writes a PE32+ image. A program
# is that tool's --elf link, and the kernel holds image 0's bytes one PE section per part at the
# program's own offsets from its first byte, at an address firmware chooses: .kimg0t the text,
# .kimg0d the data, .kimg0b the bss and heap; the later images' objects are in .kimgs. An alias
# then names the kernel's view as an offset from __kickos_image0_load_start, which is the anchor.
#
# In <kernel objects file>, the objects after a `--kernel-only` line ride the command too, and
# only the kernel's link takes them.
#
# A system names each declaration running a program as -Wl,--kickos-image-task-program=
# <k>:<task>:<record>, and each of its driver and client archives as
# -Wl,--kickos-image-system-archive=<archive>. Where <record> is `-` the program is the command's
# own target, and image k + 1 is partitioned as above. Otherwise <record> is the output of that
# program's own link, run through --program, which writes <record>.kprog, the command CMake
# computed for it and the directory it ran in, then links the command's inputs relocatable as
# <record>; image k + 1 is then that command's inputs and flags, after declaration k's objects and
# the objects marked KOS_IMAGE_SYSTEM_OBJECT, and before the system's archives. On the sim a
# program's main becomes <task>_main.

import hashlib
import json
import os
import re
import struct
import subprocess
import sys

BEGIN = '--kickos-image-link-needs-kickos_image_link'
END = '--kickos-image-link-end'
PROGRAM = '--kickos-image-program='
INIT_OBJECT = '__kickos_image_init_object'
BOTH_OBJECT = '__kickos_image_both_object'
SYSTEM_OBJECT = '__kickos_image_system_object'
SYSTEM_ARCHIVE = '--kickos-image-system-archive='
TASK_OBJECT = '__kickos_image_task_object'
RESTARTS_OBJECT = '__kickos_image_task_restarts'
HEADER = 'kickos_image_header'
REENT_CARVE = '__kickos_reent_carve'
TEMPLATE = '--kickos-image-template='
TASK_PROGRAM = '--kickos-image-task-program='
RECORD = '.kprog'
FRAGMENTS = ('kickos_images.ld', 'kickos_images_code.ld', 'kickos_images_asserts.ld',
             'kickos_images_tdata.ld', 'kickos_images_table.ld', 'kickos_images_windows.ld')
PIN_LINKS = 4
X86_LINK = 'x86_64-link.sh'
KERNEL_ONLY = '--kernel-only'
PAGE = 0x1000
PT_LOAD = 1
PF_X = 1
PF_W = 2
SHF_ALLOC = 2
SHF_TLS = 0x400
SHT_NOBITS = 8


def fail(msg):
    sys.stderr.write('image-link: ' + msg + '\n')
    sys.exit(1)


def image_file(out, n, ext):
    """Image n's own ELF or map, in the output's per-image folder."""
    return os.path.join(out + '.kimg', 'image%d.%s' % (n, ext))


def sweep_image_maps(kimg):
    """Removes the image maps a previous link left in `kimg`."""
    for name in os.listdir(kimg):
        if re.fullmatch(r'image\d+\.map', name):
            os.remove(os.path.join(kimg, name))


def wl_words(tok):
    return tok[len('-Wl,'):].split(',')


def marks(nm, path):
    """{marker: value} of the image markers `path` defines."""
    r = subprocess.run([nm, '--defined-only', path], capture_output=True, text=True)
    if r.returncode != 0:
        fail('nm refused ' + path + ': ' + r.stderr.strip())
    own = {}
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in (INIT_OBJECT, BOTH_OBJECT, TASK_OBJECT,
                                            RESTARTS_OBJECT, SYSTEM_OBJECT):
            own[parts[2]] = int(parts[0], 16)
    return own


def take_programs(cmd):
    """({k: (task, record)} of the declarations running a program, the command without them):
    record is `-` where the program is the command's own target, else the path of that
    program's own link, whose command it recorded beside it. Key None holds the system's
    archives, which a program's image takes after the program's own inputs."""
    programs = {}
    rest = []
    archives = []
    for t in cmd:
        if t.startswith('-Wl,' + SYSTEM_ARCHIVE):
            archives.append(t[len('-Wl,' + SYSTEM_ARCHIVE):])
            continue
        if t.startswith('-Wl,' + TASK_PROGRAM):
            row = t[len('-Wl,' + TASK_PROGRAM):].split(':', 2)
            if len(row) != 3 or not row[0].isdigit() or not row[1] or not row[2]:
                fail('a program row is not <declaration>:<task>:<record>: ' + t)
            programs[int(row[0])] = (row[1], row[2])
            continue
        rest.append(t)
    if programs:
        programs[None] = archives
    return programs, rest


def recorded(record):
    """The link command the program whose own link wrote `record` recorded beside it, and the
    directory it ran in."""
    path = record + RECORD
    try:
        with open(path) as fh:
            saved = json.load(fh)
    except (OSError, ValueError) as e:
        fail('no recorded program link at ' + path + ': ' + str(e))
    cmd = saved['command']
    if os.path.basename(cmd[0]) == X86_LINK:
        cmd = cmd[:1] + cmd[5:]
    if any(t.startswith('-Wl,') and BEGIN in wl_words(t) for t in cmd):
        fail(record + ' links the kernel, so it is an image of its own and no program')
    _, cmd = take_template(cmd)
    return [t for t in cmd if not t.startswith('-Wl,' + PROGRAM)], saved['cwd']


def program_parts(nm, parts, image_inputs, programs, flags, kernel_only=()):
    """Each program image of `parts` replaced by its program's own recorded inputs and its
    declaration's objects, and its flags by the program's, in `flags` ({n: (both, image_only)}).
    A program image's inputs are the program's, never the command's."""
    if not programs:
        return {}
    own = {}
    system = []
    for t in image_inputs:
        if t.startswith('-') or t.endswith('.a'):
            continue
        marked = marks(nm, t)
        if TASK_OBJECT in marked:
            own.setdefault(marked[TASK_OBJECT], []).append(t)
        elif SYSTEM_OBJECT in marked:
            system.append(t)
    archives = programs.get(None, [])
    names = {}
    for k, (task, record) in sorted((k, v) for k, v in programs.items() if k is not None):
        n = k + 1
        if n >= len(parts):
            fail('program row %d names no task image of the link' % k)
        names[n] = task
        if record == '-':
            continue
        cmd, cwd = recorded(record)
        if os.path.realpath(cwd) != os.path.realpath(os.getcwd()):
            fail(record + ' was linked in ' + cwd + ', not in ' + os.getcwd())
        _, both, image_only, _, inputs, _, _ = classify(cmd)
        inputs = [t for t in inputs if os.path.abspath(t) not in kernel_only]
        # Ahead of the program's archives, which resolve what the declaration names.
        parts[n] = own.get(k, []) + system + inputs + archives
        flags[n] = (both, image_only)
    return names


def program_main(argv, cmd):
    """A program's own link: its command recorded beside the output, then linked relocatable
    with what only an image's final link states left out."""
    host = argv[:1] == ['--host']
    if host:
        argv = argv[1:]
    if len(argv) != 1:
        fail('usage: image-link.py --program [--host] <names> --link <link...>')
    partial_script = os.path.join(os.path.dirname(os.path.abspath(argv[0])), 'image_partial.lds')
    out = None
    for i, t in enumerate(cmd):
        if t == '-o' and i + 1 < len(cmd):
            out = cmd[i + 1]
    if out is None:
        fail('the program\'s link names no -o')
    if any(t.startswith('-Wl,') and BEGIN in wl_words(t) for t in cmd):
        fail(out + ' links the kernel, so it is an image of its own and no program')
    record = out + RECORD
    with open(record + '.new', 'w') as fh:
        json.dump({'command': cmd, 'cwd': os.getcwd()}, fh)
    os.replace(record + '.new', record)
    driver = cmd[:1]
    rest = cmd
    if os.path.basename(cmd[0]) == X86_LINK:
        driver = cmd[:1] + ['--elf'] + cmd[1:5]
        rest = cmd[:1] + cmd[5:]
    _, rest = take_template(rest)
    rest = [t for t in rest if not t.startswith('-Wl,' + PROGRAM)]
    cc, both, image_only, _, inputs, _, _ = classify(rest)
    # A compiler driver adds a script of its own unless it reads -T itself; x86_64's tool takes
    # one by -Wl alone.
    script = ['-T', partial_script]
    if os.path.basename(cmd[0]) == X86_LINK:
        script = ['-Wl,-T,' + partial_script]
    relocatable = ['-Wl,-r', '-Wl,-d', '-Wl,--no-gc-sections'] + script
    if host:
        relocatable = ['-r']
    run(driver + both + image_only + relocatable + inputs + ['-o', out],
        'the relocatable link of program ' + os.path.basename(out))


def partition(nm, image_inputs):
    """[image inputs]: the init's, which an object marks (KOS_IMAGE_INIT_OBJECT,
    <kickos/sys/image.h>), and one per declared task. The init's objects are image 0's, an
    object marked KOS_IMAGE_TASK_OBJECT(k) is image k + 1's alone, the objects marked for every
    image, every archive and every flag are every image's, and every other object is every task
    image's. Each image keeps the command's order."""
    kinds = []
    init = False
    tasks = 0
    restarts = set()
    for t in image_inputs:
        if t.startswith('-') or t.endswith('.a'):
            kinds.append(('all', None))
            continue
        own = marks(nm, t)
        if INIT_OBJECT in own:
            init = True
            kinds.append(('init', None))
        elif BOTH_OBJECT in own:
            kinds.append(('all', None))
        elif TASK_OBJECT in own:
            tasks = max(tasks, own[TASK_OBJECT] + 1)
            kinds.append(('task', own[TASK_OBJECT]))
            if RESTARTS_OBJECT in own:
                restarts.add(own[TASK_OBJECT] + 1)
        else:
            kinds.append(('tasks', None))
    if not init:
        fail('no input marks itself the init\'s image (KOS_IMAGE_INIT_OBJECT): the link does '
             'not link KickOS::kernel')
    # With no system target, one image beside the init's, whose own link refuses it.
    tasks = max(tasks, 1)
    parts = [[] for _ in range(tasks + 1)]
    for t, (kind, k) in zip(image_inputs, kinds):
        for n in range(tasks + 1):
            if kind == 'all' or (kind == 'init' and n == 0) or (kind == 'tasks' and n > 0) \
                    or (kind == 'task' and n == k + 1):
                parts[n].append(t)
    return parts, restarts


def take_template(cmd):
    """(the image template the command names, or None; the command without it)."""
    template = None
    rest = []
    for t in cmd:
        if t.startswith('-Wl,' + TEMPLATE):
            if template is not None:
                fail('the link names two image templates')
            template = t[len('-Wl,' + TEMPLATE):]
            continue
        rest.append(t)
    return template, rest


def read_template(path):
    """({part: text}, (enforced, pow2, granule)) of the image template at `path`."""
    with open(path) as fh:
        text = fh.read()
    m = re.search(r'KIMGRULE\(([^,]*),([^,]*),([^)]*)\)', text)
    if m is None:
        fail(path + ': no KIMGRULE')

    def number(word):
        word = word.strip().strip('()').strip()
        try:
            return int(word, 0)
        except ValueError:
            fail(path + ': KIMGRULE holds no number but ' + word)
    rule = (number(m.group(1)), number(m.group(2)), number(m.group(3)))
    text = text[:m.start()] + text[m.end():]
    pieces = re.split(r'KIMGPART\((\w+)\)', text)
    parts = {}
    for i in range(1, len(pieces) - 1, 2):
        parts[pieces[i]] = pieces[i + 1].strip()
    for want in ('base', 'code', 'image', 'tls', 'tdata', 'table', 'asserts'):
        if want not in parts:
            fail(path + ': no KIMGPART(' + want + ')')
    return parts, rule


def instance(text, n, align=0, heap='0', base='', restarts=False):
    return (text.replace('KIMGN', str(n)).replace('KIMGALIGN', '0x%x' % align)
            .replace('KIMGHEAP', heap).replace('KIMGBASE', base)
            .replace('KIMGTPL', str(int(restarts))))


def write_fragments(kimg, parts, count, order, aligns, final, restarts):
    """The fragments of a link holding `count` images: every image's code span in image order,
    and its window placed in `order`, the first at the template's base, each based on aligns[n],
    an image in `restarts` keeping its data's template; the asserts only in the `final` link.
    Where the template has a window part, the windows are a fragment of their own, in `order`."""
    tasks = list(range(1, count))
    later = list(range(2, count))
    images = []
    window_parts = []
    asserts = []
    for at, n in enumerate(order):
        heap = 'KICKOS_USER_HEAP_SIZE'
        if n == 0:
            heap = '0'
        base = ''
        if at == 0:
            base = parts['base']
        images.append(instance(parts['image'], n, aligns[n], heap, base, n in restarts))
        if 'window' in parts:
            window_parts.append(instance(parts['window'], n, aligns[n], heap, base, n in restarts))
        if n > 1:
            images.append(instance(parts['tls'], n))
        if final:
            asserts.append(instance(parts['asserts'], n))
    texts = {
        'kickos_images.ld': images,
        'kickos_images_code.ld': [instance(parts['code'], n, restarts=n in restarts)
                                  for n in range(count)],
        'kickos_images_asserts.ld': asserts,
        'kickos_images_tdata.ld': [instance(parts['tdata'], n) for n in later],
        'kickos_images_table.ld': [instance(parts['table'], n) for n in tasks],
        'kickos_images_windows.ld': window_parts,
    }
    for name in FRAGMENTS:
        with open(os.path.join(kimg, name), 'w') as fh:
            fh.write('/* Written by tools/image-link.py. */\n' + '\n'.join(texts[name]) + '\n')


def windows(nm, path, count):
    """{n: window size} of the `count` images the link at `path` placed."""
    r = subprocess.run([nm, path], capture_output=True, text=True)
    if r.returncode != 0:
        fail('nm refused ' + path + ': ' + r.stderr.strip())
    at = {}
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        m = re.fullmatch(r'_*kickos_image(\d+)_window_(start|end)', parts[2])
        if m is not None:
            at[(int(m.group(1)), m.group(2))] = int(parts[0], 16)
    sizes = {}
    for n in range(count):
        if (n, 'start') not in at or (n, 'end') not in at:
            fail(path + ': image %d states no window' % n)
        sizes[n] = at[(n, 'end')] - at[(n, 'start')]
    return sizes


def classify(cmd):
    """(compiler, both, image_only, final_only, image_inputs, kernel_inputs, out)."""
    both, image_only, final_only = [], [], []
    image_inputs, kernel_inputs = [], []
    out = None
    in_kernel = False
    i = 1
    while i < len(cmd):
        t = cmd[i]
        if t.startswith('-Wl,') and BEGIN in wl_words(t):
            if in_kernel:
                fail('a kernel group opens inside another')
            in_kernel = True
            i += 1
            continue
        if t.startswith('-Wl,') and END in wl_words(t):
            if not in_kernel:
                fail('a kernel group closes that never opened')
            in_kernel = False
            i += 1
            continue
        if in_kernel:
            kernel_inputs.append(t)
            i += 1
            continue
        if t == '-o':
            out = cmd[i + 1]
            i += 2
            continue
        if t == '-T':
            final_only += [t, cmd[i + 1]]
            i += 2
            continue
        if t.startswith('-Wl,'):
            words = wl_words(t)
            head = words[0]
            if head in ('-T', '--script') or head.startswith('-Map') \
                    or head.startswith('--defsym') or head.startswith('--orphan-handling'):
                final_only.append(t)
            elif head in ('-u', '--undefined', '--require-defined', '--wrap') \
                    or head.startswith(('--undefined=', '--require-defined=', '--wrap=')):
                image_only.append(t)
            elif head in ('--start-group', '--end-group', '-(', '-)'):
                image_inputs.append(t)
            elif head == '--gc-sections' or head.startswith('--image-base'):
                final_only.append(t)
            else:
                both.append(t)
            i += 1
            continue
        if t.startswith('-l'):
            image_inputs.append(t)
            i += 1
            continue
        if t.startswith('-'):
            both.append(t)
            i += 1
            continue
        if t.endswith('.ld'):
            final_only.append(t)
        else:
            image_inputs.append(t)
        i += 1
    if in_kernel:
        fail('the kernel group never closes')
    # Every kernel group the command carries, as one, so no order between them matters.
    kernel_inputs = ['-Wl,--start-group'] + kernel_inputs + ['-Wl,--end-group']
    if out is None:
        fail('the link command names no -o')
    return cmd[0], both, image_only, final_only, image_inputs, kernel_inputs, out


def run(argv, what, moves=True):
    """Runs `argv`; with `moves` False, ld's warnings that a section moved between its passes
    are dropped, which a link made only to be measured raises."""
    if moves:
        r = subprocess.run(argv)
    else:
        r = subprocess.run(argv, stderr=subprocess.PIPE, text=True)
        for line in r.stderr.splitlines():
            if not re.search(r'warning: start of section \S+ changed by', line):
                sys.stderr.write(line + '\n')
    if r.returncode != 0:
        fail(what + ' failed')


def code_end(nm, path):
    """The value of _code_used_end the link at `path` defines, or None."""
    r = subprocess.run([nm, '--defined-only', path], capture_output=True, text=True)
    if r.returncode != 0:
        fail('nm refused ' + path + ': ' + r.stderr.strip())
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == '_code_used_end':
            return int(parts[0], 16)
    return None


def symbols(nm, path, flags):
    r = subprocess.run([nm] + flags + [path], capture_output=True, text=True)
    if r.returncode != 0:
        fail('nm refused ' + path + ': ' + r.stderr.strip())
    rows = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            rows.append((parts[-2], parts[-1]))
    return rows


def host_split(image_inputs):
    """(image inputs, host libraries): a host library is one per process, the final link's."""
    image, libs = [], []
    for t in image_inputs:
        if t.startswith('-l') or re.search(r'\.so(\.[0-9]+)*$', t):
            libs.append(t)
        else:
            image.append(t)
    return image, libs


def host_image(cc, objcopy, nm, flags, inputs, names_file, name, index, raw, obj, image_map,
               entry=None):
    """Links one sim image to <obj>: its own inputs, every symbol local but its two names, its
    main renamed `entry` or after the image."""
    for stale in (raw, obj):
        if os.path.exists(stale):
            os.remove(stale)
    image_map_flag = []
    if image_map:
        image_map_flag = ['-Wl,-Map=' + image_map]
    run([cc] + flags + ['-r'] + image_map_flag + inputs + ['-o', raw],
        'the partial link of image ' + name)
    header = '__kickos_image' + str(index) + '_header'
    if entry is None:
        entry = name + '_main'
        if index > 1:
            entry = name + '_main' + str(index)
    renames = {HEADER: header, 'main': entry}
    with open(names_file) as fh:
        for line in fh:
            line = line.split('#', 1)[0].split()
            if len(line) == 2:
                renames[line[0]] = '__kickos_image' + str(index) + '_' + line[1]
    names = raw + '.names'
    with open(names, 'w') as fh:
        for generic, own_name in sorted(renames.items()):
            fh.write(generic + ' ' + own_name + '\n')
    # objcopy makes no GNU unique symbol local, so each becomes weak first.
    weaken = []
    for kind, sym in symbols(nm, raw, ['--defined-only']):
        if kind == 'u':
            weaken.append('--weaken-symbol=' + sym)
    weak = raw + '.weak.o'
    # GNU ld drops a COMDAT group whose signature another input already brought, local or not,
    # which would leave the image's references in a discarded section.
    run([objcopy] + weaken + ['--redefine-syms=' + names, '--remove-section=.group', raw, weak],
        'objcopy over image ' + name)
    run([objcopy, '--keep-global-symbol=' + header, '--keep-global-symbol=' + entry, weak, obj],
        'objcopy over image ' + name)
    return renames


def host_table(cc, kimg, count):
    """The image table the kernel reads, and each image's empty constructor window: the host
    runtime runs every constructor before the kernel boots."""
    src = os.path.join(kimg, 'image_table.s')
    out = os.path.join(kimg, 'image_table.o')
    lines = ['.pushsection .data.rel.ro.kickos_image_table,"aw"', '.balign 8',
             '.globl __kickos_image_table', '__kickos_image_table:']
    for i in range(count):
        lines.append('.quad __kickos_image' + str(i) + '_header')
    lines += ['.globl __kickos_image_table_end', '__kickos_image_table_end:', '.popsection',
              '.pushsection .kickos_app_init_array,"a"']
    for i in range(count):
        for edge in ('start', 'end'):
            label = '__kickos_image' + str(i) + '_app_init_array_' + edge
            lines += ['.globl ' + label, label + ':']
    lines += ['.popsection', '.section .note.GNU-stack,"",@progbits']
    with open(src, 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    run([cc, '-c', '-x', 'assembler', src, '-o', out], 'the image table')
    return out


def host_main(own, cmd, programs):
    partial = '--partial' in own
    own = [a for a in own if a != '--partial']
    if len(own) < 3:
        fail('usage: image-link.py --host [--partial] <objcopy> <nm> <names> [<kernel entry>...] '
             '--link <link...>')
    objcopy, nm, names_file = own[:3]
    entries = set(own[3:])
    cc, both, image_only, final_only, image_inputs, kernel_inputs, out = classify(cmd)
    image, libs = host_split(image_inputs)
    name = os.path.splitext(os.path.basename(out))[0]
    if partial:
        host_image(cc, objcopy, nm, both + image_only, image, names_file, name, 0, out + '.raw.o',
                   out, None)
        return
    parts, _ = partition(nm, image)
    flags = {}
    tasks = program_parts(nm, parts, image, programs, flags)
    for n in flags:
        parts[n], more = host_split(parts[n])
        libs += [lib for lib in more if lib not in libs]
    kimg = out + '.kimg'
    os.makedirs(kimg, exist_ok=True)
    sweep_image_maps(kimg)
    if os.path.exists(out):
        os.remove(out)

    kernel_defined = set()
    for path in [t for t in kernel_inputs if os.path.isfile(t)]:
        kernel_defined |= {n for _, n in symbols(nm, path, ['-g', '--defined-only'])}
    objs = []
    for n, inputs in enumerate(parts):
        obj = os.path.abspath(os.path.join(kimg, 'kickos_image%d.o' % n))
        # A program's main is its task's, <task>_main.
        entry = None
        if n in tasks:
            entry = tasks[n] + '_main'
        own_both, own_only = flags.get(n, (both, image_only))
        renames = host_image(cc, objcopy, nm, own_both + own_only, inputs, names_file, name, n,
                             os.path.join(kimg, 'image%d.raw.o' % n), obj,
                             image_file(out, n, 'map'), entry)
        allowed = set(renames.values()) | entries
        undefined = {s for _, s in symbols(nm, obj, ['-u'])}
        bound = sorted((undefined & kernel_defined) - allowed)
        if bound:
            fail('image %d names what only the kernel defines: ' % n + ' '.join(bound))
        objs.append(obj)

    table = host_table(cc, kimg, len(objs))
    run([cc] + both + final_only + objs + [table, '-o', out] + kernel_inputs + libs,
        'the final link')


def digest_of(paths):
    digest = hashlib.sha256()
    for path in paths:
        with open(path, 'rb') as fh:
            digest.update(fh.read())
    return int.from_bytes(digest.digest()[:4], 'little') | 1


def load_segments(path):
    """[(vaddr, paddr, filesz, flags, bytes, nobits)] of an ELF64 little-endian file's PT_LOADs,
    each from its first allocated section, since a segment may also load the file's own headers;
    nobits lists the (address, size) of each allocated SHT_NOBITS section it spans but TLS's."""
    with open(path, 'rb') as fh:
        elf = fh.read()
    if elf[:4] != b'\x7fELF' or elf[4] != 2 or elf[5] != 1:
        fail(path + ': not an ELF64 little-endian file')
    phoff, shoff = struct.unpack_from('<QQ', elf, 0x20)
    phentsize, phnum, shentsize, shnum = struct.unpack_from('<HHHH', elf, 0x36)
    starts = []
    spans = []
    for i in range(shnum):
        _, sh_type, sh_flags, sh_addr, _, sh_size = struct.unpack_from(
            '<IIQQQQ', elf, shoff + i * shentsize)
        if sh_flags & SHF_ALLOC:
            starts.append(sh_addr)
            if sh_type == SHT_NOBITS and not sh_flags & SHF_TLS and sh_size:
                spans.append((sh_addr, sh_size))
    loads = []
    for i in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _ = struct.unpack_from(
            '<IIQQQQQQ', elf, phoff + i * phentsize)
        if p_type != PT_LOAD or p_memsz == 0:
            continue
        inside = [a for a in starts if p_vaddr <= a < p_vaddr + p_memsz]
        if not inside:
            fail(path + ': a loadable segment at 0x%x holds no allocated section' % p_vaddr)
        skip = min(inside) - p_vaddr
        if skip > p_filesz:
            fail(path + ': the segment at 0x%x starts past its file bytes' % p_vaddr)
        nobits = sorted(sp for sp in spans if p_vaddr <= sp[0] < p_vaddr + p_memsz)
        loads.append((p_vaddr + skip, p_paddr + skip, p_filesz - skip, p_flags,
                      elf[p_offset + skip:p_offset + p_filesz], nobits))
    if not loads:
        fail(path + ': no loadable segment')
    return loads


def embed_names(path):
    """[(kind, kernel name, program name)] from <embed names>."""
    kinds = ('copy', 'least', 'most', 'same', 'alias', 'check')
    rows = []
    with open(path) as fh:
        for line in fh:
            line = line.split('#', 1)[0].split()
            if not line:
                continue
            if len(line) != 3 or line[0] not in kinds:
                fail(path + ': not `' + '|'.join(kinds) + ' <kernel name> <program name>`: '
                     + ' '.join(line))
            rows.append(tuple(line))
    return rows


def pe_fragment(kimg, ld, objcopy, program, segments):
    """x86_64: image 0's bytes as objects for a PE32+ kernel link, and the fragment placing them.
    The text, the data and the bss each start on a page of the program, and each part's section
    sits at that part's offset from the kernel's first byte of the image."""
    if len(segments) != 2 or segments[0][3] & PF_W or not segments[1][3] & PF_W \
            or not segments[1][2]:
        fail(program + ': not one text and one data segment, which the kernel\'s link mirrors')
    base = segments[0][0]
    text_end = segments[0][0] + segments[0][2]
    data, data_end = segments[1][0], segments[1][0] + segments[1][2]
    nobits = segments[1][5]
    bss = data_end
    bss_end = data_end
    if nobits:
        bss, bss_end = nobits[0][0], nobits[-1][0] + nobits[-1][1]
    if base % PAGE or data % PAGE or bss % PAGE or text_end > data or data_end > bss:
        fail(program + ': its text, data and bss do not each start on a page, in that order')
    objects = []
    for part, flags, data_bytes, size in (('text', 'readonly,code', segments[0][4], 0),
                                          ('data', 'data', segments[1][4], 0),
                                          ('bss', None, b'', bss_end - bss)):
        if part == 'bss' and size == 0:
            continue
        bin_path = os.path.join(kimg, 'image0.%s.bin' % part)
        with open(bin_path, 'wb') as fh:
            if size:
                fh.truncate(size)
            else:
                fh.write(data_bytes)
        rename = '.data=.kickos_image0_%s,alloc' % part
        if flags is not None:
            rename += ',load,contents,' + flags
        obj = os.path.abspath(os.path.join(kimg, 'image0.%s.o' % part))
        run([objcopy, '-I', 'binary', '-O', 'elf64-x86-64', '-B', 'i386:x86-64', '--strip-all',
             '--rename-section', rename, bin_path, obj], 'objcopy over image 0\'s ' + part)
        objects.append(obj)
    image = os.path.abspath(os.path.join(kimg, 'kickos_image0.o'))
    run([ld, '-m', 'elf_x86_64', '-r', '-o', image] + objects, 'the object of image 0\'s bytes')
    lines = ['/* Image 0, ' + os.path.basename(program) + ': written by tools/image-link.py. */',
             '.kimg0t BLOCK(__section_alignment__) :',
             '{',
             '    __kickos_image0_load_start = .;',
             '    __kickos_image_frames_anchor = .;',
             '    KEEP(*(.kickos_image0_text))',
             '    . = __kickos_image0_load_start + 0x%x;' % (data - base),
             '}',
             '.kimg0d BLOCK(__section_alignment__) :',
             '{',
             '    KEEP(*(.kickos_image0_data))',
             '    . = __kickos_image0_load_start + 0x%x;' % (bss - base),
             '}',
             '.kimg0b BLOCK(__section_alignment__) :',
             '{',
             '    KEEP(*(.kickos_image0_bss))',
             '    __kickos_image0_load_end = .;',
             '}',
             'ASSERT(ADDR(.kimg0d) == __kickos_image0_load_start + 0x%x' % (data - base)
             + ' && __kickos_image0_load_end == __kickos_image0_load_start + 0x%x,'
             % (bss_end - base),
             '       "KickOS: the kernel\'s link does not hold image 0 at the program\'s own '
             'offsets")']
    return image, lines, base


SPAN_NAMES = ('__kickos_app_rom_start', '__kickos_app_rom_end', '__kickos_app_sram_start',
              '__kickos_app_sram_end', '__kickos_tls_segment_start', '__kickos_tls_segment_end')


def image_spans(program, values):
    """(rom_lo, rom_hi, sram_lo, sram_hi, tls_lo, tls_hi) the program at `program` states."""
    missing = [name for name in SPAN_NAMES if name not in values]
    if missing:
        fail(program + ': states no ' + ' '.join(missing))
    spans = tuple(values[name] for name in SPAN_NAMES)
    # The kernel takes an address past the window's start for a window byte.
    if spans[2] % PAGE or spans[0] % PAGE or spans[1] > spans[2]:
        fail(program + ': its code span and its window do not each start on a page, in that order')
    return spans


def page_down(a):
    return a & ~(PAGE - 1)


def page_up(a):
    return (a + PAGE - 1) & ~(PAGE - 1)


def span_bytes(segments, lo, hi):
    """The bytes the loader puts at [lo, hi): each load segment's file bytes, zero elsewhere."""
    out = bytearray(hi - lo)
    for vaddr, _, filesz, _, data, _ in segments:
        a = max(lo, vaddr)
        b = min(hi, vaddr + filesz)
        if a < b:
            out[a - lo:b - lo] = data[a - vaddr:b - vaddr]
    return bytes(out)


def kernel_exports(rows, programs, pe, base):
    """(fragment lines, assert lines, the kernel names stated) for the embed names `rows` over
    `programs`, each (path, raw, values, segments, spans), image 0's first."""
    image0 = programs[0][2]
    lines = []
    asserts = ['/* Written by tools/image-link.py. */']
    stated = set()
    for kind, kernel_name, program_name in rows:
        if program_name not in image0:
            continue
        v = image0[program_name]
        if kind in ('least', 'most', 'same'):
            every = [p[2].get(program_name) for p in programs]
            if None in every:
                fail('image %d states no %s, which image 0 does'
                     % (every.index(None), program_name))
            if kind == 'least':
                v = min(every)
            elif kind == 'most':
                v = max(every)
            elif any(w != v for w in every):
                fail('image %d states %s at 0x%x and image 0 at 0x%x, which the kernel holds '
                     'one of' % ([w != v for w in every].index(True), program_name,
                                 every[[w != v for w in every].index(True)], v))
            lines.append('%s = 0x%x;' % (kernel_name, v))
            stated.add(kernel_name)
        elif kind == 'copy':
            lines.append('%s = 0x%x;' % (kernel_name, v))
            stated.add(kernel_name)
        elif kind == 'alias':
            if pe:
                lines.append('%s = __kickos_image0_load_start + 0x%x;' % (kernel_name, v - base))
            else:
                lines.append('%s = 0x%x + __kickos_image_kernel_alias;' % (kernel_name, v))
        else:
            for n, p in enumerate(programs):
                if program_name in p[2]:
                    w = p[2][program_name]
                    asserts.append('ASSERT(%s == 0x%x, "KickOS: image %d states %s at 0x%x and '
                                   'the kernel is linked elsewhere");'
                                   % (kernel_name, w, n, kernel_name, w))
    return lines, asserts, stated


def embed(driver, objcopy, nm, names_file, kernel_objects, kernel_only, entries, cmd, bound):
    """Each image as a program of its own, its loadable bytes embedded in the kernel's link."""
    program_script = None
    rest = []
    for t in cmd:
        if t.startswith('-Wl,' + PROGRAM):
            if program_script is not None:
                fail('the link names two program scripts')
            program_script = t[len('-Wl,' + PROGRAM):]
            continue
        rest.append(t)
    if program_script is None:
        fail('the link names no -Wl,' + PROGRAM + '<script>')
    pe = os.path.basename(driver[0]) == X86_LINK
    _, both, image_only, final_only, image_inputs, kernel_inputs, out = classify(rest)
    only = {os.path.abspath(o) for o in kernel_only}
    image_inputs = [t for t in image_inputs if os.path.abspath(t) not in only]
    parts, _ = partition(nm, image_inputs)
    flags = {}
    program_parts(nm, parts, image_inputs, bound, flags, only)
    # Both links take the command's symbol definitions. The program takes its link-time assert
    # scripts; the kernel takes its -T script and its map.
    program_own = []
    kernel_own = []
    i = 0
    while i < len(final_only):
        t = final_only[i]
        if t == '-T':
            kernel_own += [t, final_only[i + 1]]
            i += 2
            continue
        if t.startswith('-Wl,') and wl_words(t)[0].startswith('--defsym') \
                or t.startswith('-Wl,') and wl_words(t)[0] in ('--gc-sections',) \
                or t.startswith('-Wl,--orphan-handling'):
            program_own.append(t)
            kernel_own.append(t)
        elif t.endswith('.ld') and not t.startswith('-'):
            program_own.append(t)
        else:
            kernel_own.append(t)
        i += 1
    # x86_64's tool links the program as an ELF executable, and takes a script by -Wl only.
    program_driver = driver
    script_flag = ['-T']
    if pe:
        program_driver = [driver[0], '--elf'] + driver[1:]
        script_flag = []
        program_own.append('-Wl,--orphan-handling=error')

    kimg = out + '.kimg'
    os.makedirs(kimg, exist_ok=True)
    obj = os.path.abspath(os.path.join(kimg, 'kickos_image0.o'))
    source = os.path.join(kimg, 'kickos_image0.S')
    fragment = os.path.join(kimg, 'kickos_image0.ld')
    checks = os.path.join(kimg, 'kickos_image0_checks.ld')
    table = os.path.join(kimg, 'kickos_image_table.ld')
    frames = os.path.join(kimg, 'kickos_image_frames.ld')
    stale = [obj, source, fragment, checks, table, frames, out]
    for name in os.listdir(kimg):
        if re.fullmatch(r'image\d+\.(raw\.o|store\.bin|store\.S|elf|map)|kickos_image\d+\.o', name):
            stale.append(os.path.join(kimg, name))
    for path in stale:
        if os.path.exists(path):
            os.remove(path)
    if '"' in os.path.abspath(kimg) or '\n' in os.path.abspath(kimg):
        fail('refusing a build path holding a quote or a newline: ' + kimg)

    def script(path):
        if pe:
            return ['-Wl,-T,' + path]
        return script_flag + [path]

    hashed = [t for t in image_inputs + kernel_inputs if os.path.isfile(t)]
    for n in flags:
        hashed += [t for t in parts[n] if os.path.isfile(t) and t not in hashed]
    build_id = digest_of(hashed + kernel_objects + kernel_only)
    defsym = '-Wl,--defsym=__kickos_build_id=' + hex(build_id)
    partial_script = os.path.join(os.path.dirname(os.path.abspath(names_file)), 'image_partial.lds')
    programs = []
    for n, inputs in enumerate(parts):
        program = image_file(out, n, 'elf')
        raw = os.path.join(kimg, 'image%d.raw.o' % n)
        # The script's entry is root's, the init's; a task image is rooted at its header, and
        # its link holds neither root nor idle. Each task image's thread-local carve is this
        # program's own where it is this program, and nothing where it is another's.
        entry = []
        if n > 0:
            entry = ['-Wl,-e,' + HEADER, '-Wl,--defsym=__kickos_task_program=1']
        for k in range(1, len(parts)):
            carve = '0'
            if k == n:
                carve = '__kickos_tls_carve'
            entry.append('-Wl,--defsym=__kickos_image%d_tls_carve=%s' % (k, carve))
        own_both, own_only = flags.get(n, (both, image_only))
        run(program_driver + own_both + own_only + program_own + entry
            + script(program_script) + ['-Wl,-Map=' + image_file(out, n, 'map')] + inputs
            + [defsym, '-o', program], 'the link of image %d' % n)
        # A static link keeps no undefined weak symbol, so what an image leaves undefined is
        # read off a relocatable link of the same inputs.
        run(program_driver + own_both + own_only + ['-Wl,-r', '-Wl,--no-gc-sections']
            + script(partial_script) + inputs + ['-o', raw],
            'the relocatable link of image %d' % n)
        values = {}
        r = subprocess.run([nm, '--defined-only', program], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            row = line.split()
            if len(row) == 3:
                values[row[2]] = int(row[0], 16)
        if HEADER not in values:
            fail(program + ': defines no ' + HEADER)
        segments = load_segments(program)
        programs.append((program, raw, values, segments, image_spans(program, values)))

    program, _, _, segments, _ = programs[0]
    lines = ['/* Image 0, ' + os.path.basename(program) + ': written by tools/image-link.py. */']
    asm = []
    base = 0
    if pe:
        obj, lines, base = pe_fragment(kimg, driver[1], objcopy, program, segments)
    else:
        for n, (vaddr, paddr, filesz, flags, data, nobits) in enumerate(segments):
            section = '.kickos_image0_load%d' % n
            if filesz:
                bin_path = os.path.abspath(os.path.join(kimg, 'kickos_image0.load%d.bin' % n))
                if '"' in bin_path or '\n' in bin_path:
                    fail('refusing a build path holding a quote or a newline: ' + bin_path)
                with open(bin_path, 'wb') as fh:
                    fh.write(data)
                sflags = 'a'
                if flags & PF_X:
                    sflags += 'x'
                if flags & PF_W:
                    sflags += 'w'
                # No alignment directive: RISC-V's assembler pads one in code with nops it
                # leaves for the linker to relax, and the fragment places each section by
                # address.
                asm += ['.section %s,"%s",%%progbits' % (section, sflags),
                        '.incbin "%s"' % bin_path]
                lines.append('%s 0x%x : AT(0x%x) { KEEP(*(%s)) }'
                             % (section, vaddr, paddr, section))
            for m, (addr, size) in enumerate(nobits):
                lines.append('%s_bss%d 0x%x (NOLOAD) : AT(0x%x) { . += 0x%x; }'
                             % (section, m, addr, paddr + addr - vaddr, size))

    # Every later image's text and its window, each on pages of its own, in its object
    # kickos_image<n>.o, which the kernel's script places among the kernel's own pages at
    # __kickos_image<n>_store; a space maps those frames at the program's addresses.
    placed = []
    image_objs = [obj]
    for n, (program, _, _, segments, sp) in enumerate(programs[1:], 1):
        offs = []
        chunk = bytearray()
        for lo, hi in ((sp[0], sp[1]), (sp[2], sp[3])):
            offs.append((len(chunk), page_down(lo)))
            chunk += span_bytes(segments, page_down(lo), page_up(hi))
        placed.append(offs)
        bin_path = os.path.abspath(os.path.join(kimg, 'image%d.store.bin' % n))
        image_obj = os.path.abspath(os.path.join(kimg, 'kickos_image%d.o' % n))
        label = '__kickos_image%d_store' % n
        with open(bin_path, 'wb') as fh:
            fh.write(chunk)
        if pe:
            run([objcopy, '-I', 'binary', '-O', 'elf64-x86-64', '-B', 'i386:x86-64',
                 '--strip-all', '--add-symbol', label + '=.kickos_images:0,global',
                 '--rename-section', '.data=.kickos_images,alloc,load,contents,readonly,data',
                 '--set-section-alignment', '.kickos_images=%d' % PAGE, bin_path, image_obj],
                'objcopy over image %d\'s bytes' % n)
        else:
            image_source = os.path.join(kimg, 'image%d.store.S' % n)
            with open(image_source, 'w') as fh:
                fh.write('\n'.join(['.section .kickos_images,"a",%progbits', '.p2align 12',
                                    '.globl ' + label, label + ':',
                                    '.incbin "%s"' % bin_path]) + '\n')
            machine = [t for t in both if t.startswith('-m')]
            run(driver + machine + ['-c', '-x', 'assembler', image_source, '-o', image_obj],
                'the assembly of image %d\'s bytes' % n)
        image_objs.append(image_obj)

    # The kernel's table of headers, every one at the program's own address, and per image the
    # spans a space maps and where the loader put their bytes: each delta takes a program
    # address to its frame, past __kickos_image_frames_anchor where the kernel is relocated.
    head = '/* Written by tools/image-link.py. */'
    rows = [head]
    if not pe:
        rows.append('__kickos_image_frames_anchor = ABSOLUTE(0);')
    for n, (program, _, _, segments, sp) in enumerate(programs):
        if n == 0 and pe:
            deltas = ['0 - 0x%x' % base, '0 - 0x%x' % base]
        elif n == 0:
            deltas = []
            for at in (sp[0], sp[2]):
                below = [seg for seg in segments if seg[0] <= at]
                if not below:
                    fail(program + ': no load segment holds 0x%x' % at)
                vaddr, paddr = below[-1][0], below[-1][1]
                deltas.append('0x%x' % ((paddr - vaddr) & (2 ** 64 - 1)))
        else:
            deltas = []
            label = '__kickos_image%d_store' % n
            for off, va in placed[n - 1]:
                if pe:
                    deltas.append('%s - __kickos_image0_load_start + 0x%x - 0x%x'
                                  % (label, off, va))
                else:
                    deltas.append('%s - ADDR(.kickos_images) + LOADADDR(.kickos_images)'
                                  ' + 0x%x - 0x%x' % (label, off, va))
        rows += ['/* Image %d, %s. */' % (n, os.path.basename(program)),
                 'QUAD(0x%x); QUAD(0x%x); QUAD(%s);' % (sp[0], sp[1], deltas[0]),
                 'QUAD(0x%x); QUAD(0x%x); QUAD(%s);' % (sp[2], sp[3], deltas[1]),
                 'QUAD(0x%x); QUAD(0x%x);' % (sp[4], sp[5])]
    with open(frames, 'w') as fh:
        fh.write('\n'.join(rows) + '\n')
    with open(table, 'w') as fh:
        fh.write('\n'.join([head] + ['QUAD(0x%x);' % v[HEADER] for _, _, v, _, _ in programs])
                 + '\n')

    embed_file = os.path.join(os.path.dirname(os.path.abspath(names_file)), 'embed_names.txt')
    exported, asserts, stated = kernel_exports(embed_names(embed_file), programs, pe, base)
    lines += exported
    with open(fragment, 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    with open(checks, 'w') as fh:
        fh.write('\n'.join(asserts) + '\n')
    if not pe:
        with open(source, 'w') as fh:
            fh.write('\n'.join(asm) + '\n')
        machine = [t for t in both if t.startswith('-m')]
        run(driver + machine + ['-c', '-x', 'assembler', source, '-o', obj],
            'the assembly of image 0\'s bytes')

    # The fragment's directory before the script that includes it: ld reads a -T script where
    # the command names it.
    kernel_link = (driver + ['-Wl,-L,' + os.path.abspath(kimg)] + both + kernel_own + [defsym]
                   + image_objs + kernel_objects + kernel_only + ['-o', out]
                   + kernel_inputs)
    if pe:
        # ld says it drops each user address the fragment states from the PE32+ symbol table;
        # the map keeps them, so only those lines are dropped.
        r = subprocess.run(kernel_link, stderr=subprocess.PIPE, text=True)
        for line in r.stderr.splitlines(True):
            m = re.search(r": stripping non-representable symbol '([^']*)'", line)
            if m is None or m.group(1) not in stated:
                sys.stderr.write(line)
        if r.returncode != 0:
            fail('the kernel\'s link failed')
    else:
        run(kernel_link, 'the kernel\'s link')

    # What a program's own link left undefined, a weak reference, is what one link of the
    # kernel and the image would have bound.
    taken = {name for _, name in symbols(nm, out, ['-g', '--defined-only'])}
    for n, (_, raw, values, _, _) in enumerate(programs):
        left = {name for _, name in symbols(nm, raw, ['-u'])} - set(values)
        bound = sorted((left & taken) - entries)
        if bound:
            os.remove(out)
            fail('image %d leaves undefined what only the kernel defines: ' % n
                 + ' '.join(bound))


def main(argv):
    usage = ('usage: image-link.py [--embed] <objcopy> <nm> <names> <kernel objects> '
             '[<kernel entry>...] --link <link...>')
    if '--link' not in argv:
        fail(usage)
    sep = argv.index('--link')
    own, cmd = argv[1:sep], argv[sep + 1:]
    if '--program' in own:
        program_main([a for a in own if a != '--program'], cmd)
        return
    programs, cmd = take_programs(cmd)
    if own[:1] == ['--host']:
        if not cmd:
            fail(usage)
        if '--partial' not in own and not any(
                t.startswith('-Wl,') and BEGIN in wl_words(t) for t in cmd):
            os.execvp(cmd[0], cmd)
        host_main(own[1:], cmd, programs)
        return
    embedding = own[:1] == ['--embed']
    if embedding:
        own = own[1:]
    if len(own) < 4 or not cmd:
        fail(usage)
    objcopy, nm, names_file, objects_file = own[:4]
    entries = set(own[4:])
    kernel_objects = []
    kernel_only = []
    into = kernel_objects
    with open(objects_file) as fh:
        for line in fh:
            line = line.strip()
            if line == KERNEL_ONLY:
                into = kernel_only
            elif line:
                into.append(line)

    if not any(t.startswith('-Wl,') and BEGIN in wl_words(t) for t in cmd):
        os.execvp(cmd[0], cmd)

    if embedding:
        driver = cmd[:1]
        if os.path.basename(cmd[0]) == X86_LINK:
            driver = cmd[:5]
            cmd = cmd[:1] + cmd[5:]
        embed(driver, objcopy, nm, names_file, kernel_objects, kernel_only, entries, cmd, programs)
        return

    template, cmd = take_template(cmd)
    cc, both, image_only, final_only, image_inputs, kernel_inputs, out = classify(cmd)
    parts, restarts = partition(nm, image_inputs)
    flags = {}
    program_parts(nm, parts, image_inputs, programs, flags)
    kimg = out + '.kimg'
    os.makedirs(kimg, exist_ok=True)
    sweep_image_maps(kimg)
    if os.path.exists(out):
        os.remove(out)
    if template is None:
        fail('the link names no -Wl,' + TEMPLATE + '<template>')

    generic = {}
    with open(names_file) as fh:
        for line in fh:
            line = line.split('#', 1)[0].split()
            if len(line) == 2:
                generic[line[0]] = line[1]
    partial_script = os.path.join(os.path.dirname(os.path.abspath(names_file)), 'image_partial.lds')
    # Two images may each carry an inline function: a COMDAT group the final link would keep in
    # one image only, leaving the other's references in a discarded section.
    groups = ['-Wl,--force-group-allocation']
    objs = []
    allowed = set()
    undefined = set()
    for n, inputs in enumerate(parts):
        raw = os.path.join(kimg, 'image%d.raw.o' % n)
        obj = os.path.abspath(os.path.join(kimg, 'kickos_image%d.o' % n))
        names = os.path.join(kimg, 'image%d.names' % n)
        for stale in (raw, obj):
            if os.path.exists(stale):
                os.remove(stale)
        # rx-elf collects garbage by default, which in a link no entry roots drops everything.
        # A common symbol stays global through objcopy; -d allocates it here so it is made local.
        own_both, own_only = flags.get(n, (both, image_only))
        run([cc] + own_both + own_only + groups
            + ['-Wl,-r', '-Wl,-d', '-Wl,--no-gc-sections',
               '-Wl,-Map=' + image_file(out, n, 'map'), '-T', partial_script]
            + inputs + ['-o', raw], 'the partial link of image %d' % n)
        prefix = ''
        if any(name == '_' + HEADER for _, name in symbols(nm, raw, ['-g', '--defined-only'])):
            prefix = '_'
        own = '__kickos_image%d_' % n
        with open(names, 'w') as fh:
            for name, field in sorted(generic.items()):
                fh.write(prefix + name + ' ' + prefix + own + field + '\n')
        # Each header keeps a name of its own, image 0's the generic one. So does the libc
        # carve the image's user library defines, which the chip script reads per image.
        header = prefix + HEADER
        keep = ['--keep-global-symbol=' + header]
        if n > 0:
            header = prefix + 'kickos_image%d_header' % n
            keep = ['--redefine-sym=' + prefix + HEADER + '=' + header,
                    '--keep-global-symbol=' + header]
        carve = prefix + own + 'reent_carve'
        keep += ['--redefine-sym=' + prefix + REENT_CARVE + '=' + carve,
                 '--keep-global-symbol=' + carve]
        run([objcopy, '--redefine-syms=' + names] + keep + [raw, obj],
            'objcopy over image %d' % n)
        objs.append(obj)
        allowed |= {prefix + own + f for f in generic.values()}
        allowed |= {prefix + '__kickos_build_id'} | {prefix + e for e in entries}
        undefined |= {name for _, name in symbols(nm, obj, ['-u'])}

    build_id = digest_of(objs + kernel_objects + [t for t in kernel_inputs if os.path.isfile(t)])

    def final_link(extra=(), moves=True):
        run([cc, '-Wl,-L,' + os.path.abspath(kimg)] + both + final_only + list(extra)
            + ['-Wl,--defsym=' + prefix + '__kickos_build_id=' + hex(build_id)] + objs
            + kernel_objects + ['-o', out] + kernel_inputs, 'the final link', moves)

    pin = 0
    frag, rule = read_template(template)
    enforced, pow2, granule = rule
    count = len(parts)
    placed = list(range(count))
    aligns = {n: granule for n in placed}
    if enforced and pow2:
        write_fragments(kimg, frag, count, placed, aligns, False, restarts)
        final_link(moves=False)
        sizes = windows(nm, out, count)
        end = code_end(nm, out)
        if end is not None:
            # Where code runs from the RAM the data follows, the data stays where the code
            # ended in the last link, so no section start moves between passes; a window
            # placement that relaxes the code less moves the end, and the link repeats.
            pin = (end + 7) & ~7
        os.remove(out)
        placed = sorted(placed, key=lambda n: (-sizes[n], n))
        aligns = sizes
    write_fragments(kimg, frag, count, placed, aligns, True, restarts)
    if pin == 0:
        final_link()
    else:
        for _ in range(PIN_LINKS):
            final_link(['-Wl,--defsym=__kickos_data_pin=' + hex(pin)], moves=False)
            end = code_end(nm, out)
            if end <= pin:
                break
            pin = (end + 7) & ~7
            os.remove(out)
        else:
            fail('the code end did not settle in ' + str(PIN_LINKS) + ' links')

    taken = {name for _, name in symbols(nm, out, ['-g', '--defined-only'])}
    bound = sorted((undefined & taken) - allowed)
    if bound:
        os.remove(out)
        fail('the final link gave an image what only another image or the kernel defines: '
             + ' '.join(bound))


if __name__ == '__main__':
    main(sys.argv)
