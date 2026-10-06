# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No emulator gate is registered under a board name. kickos_add_qemu_test is total over the
# fleet, so the only condition a call site owes is the fact its claim rests on: an arch, a
# Kconfig symbol, the emulated machine, the existence of an image. A board name in that place
# is a list of where the gate was once run, and a new preset that can run it is left out in
# silence.
#
# A call is refused when it sits under a condition naming a board, when an earlier condition
# naming a board returned out of the fragment, or when its condition reads a variable set
# under a condition naming a board.
#
#   python3 tests/static/check_qemu_gates_derived.py <cmake file>...

import re
import sys

OPEN_RE = re.compile(r'^\s*if\s*\((.*)$', re.I)
ELSE_RE = re.compile(r'^\s*(?:elseif|else)\s*\((.*)$', re.I)
CLOSE_RE = re.compile(r'^\s*endif\s*\(', re.I)
RETURN_RE = re.compile(r'^\s*return\s*\(', re.I)
SET_RE = re.compile(r'^\s*(?:set|list\s*\(\s*(?:APPEND|PREPEND|INSERT))\s*\(?\s*([A-Za-z_][A-Za-z0-9_]*)',
                    re.I)
CALL_RE = re.compile(r'^\s*kickos_add_qemu_test\s*\(', re.I)
# The board's name, and the CTest prefix the integration directory spells out of it.
BOARD_RE = re.compile(r'\b(KICKOS_BOARD|KICKOS_BOARD_ID|_tag)\b')


def uncomment(line):
    quoted = False
    for i, c in enumerate(line):
        if c == '"':
            quoted = not quoted
        elif c == '#' and not quoted:
            return line[:i]
    return line


def statements(text):
    """(first line, text) of each command, its continuation lines joined, comments dropped."""
    out = []
    cur = ''
    first = 0
    depth = 0
    for n, line in enumerate(text.split('\n'), 1):
        code = uncomment(line)
        if not cur.strip():
            first = n
        cur += ' ' + code
        depth += code.count('(') - code.count(')')
        if depth <= 0:
            out.append((first, cur.strip()))
            cur = ''
            depth = 0
    return out


def names_board(cond, tainted):
    if BOARD_RE.search(cond):
        return True
    for word in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', cond):
        if word in tainted:
            return True
    return False


def problems(path, text):
    out = []
    stack = []        # per open if: does any condition of its chain name a board
    tainted = set()
    guarded = 0       # line of a board-conditioned return() already passed, or 0
    for n, st in statements(text):
        m = OPEN_RE.match(st)
        if m:
            stack.append(names_board(m.group(1), tainted))
            continue
        m = ELSE_RE.match(st)
        if m and stack:
            stack[-1] = stack[-1] or names_board(m.group(1), tainted)
            continue
        if CLOSE_RE.match(st) and stack:
            stack.pop()
            continue
        under = any(stack)
        if RETURN_RE.match(st) and under and not guarded:
            guarded = n
            continue
        m = SET_RE.match(st)
        if m and (under or BOARD_RE.search(st[m.end():])):
            tainted.add(m.group(1))
            continue
        if CALL_RE.match(st):
            if under:
                out.append('%s:%d: an emulator gate registered under a board name' % (path, n))
            elif guarded:
                out.append('%s:%d: an emulator gate past a board-name return at line %d'
                           % (path, n, guarded))
    return out


PLANTED_BAD = '''if(KICKOS_BOARD STREQUAL "qemu-arm64" OR KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(TARGET a SCRIPT a.sh)
endif()
if(KICKOS_MEMORY_ENFORCED)
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME b COMMAND b.sh)
  elseif(KICKOS_QEMU_MPS2
         OR KICKOS_BOARD STREQUAL "qemu-riscv")
    kickos_add_qemu_test(TARGET b SCRIPT b.sh)
  endif()
endif()
set(_c OFF)
if(KICKOS_BOARD_ID STREQUAL "qemu-arm64")
  set(_c ON)
endif()
if(_c)
  kickos_add_qemu_test(TARGET c SCRIPT c.sh)
endif()
if(NOT _tag STREQUAL "microbit")
  return()
endif()
kickos_add_qemu_test(TARGET d SCRIPT d.sh)
'''
PLANTED_GOOD = '''if(NOT TARGET a)
  return()
endif()
kickos_add_qemu_test(TARGET a SCRIPT a.sh)
if(KICKOS_ARCH MATCHES "^(armv8a|rv64imac)$" AND KICKOS_KERNEL_CORES GREATER 1)
  kickos_add_qemu_test(NAME ${_tag}_b TARGET b SCRIPT b.sh)
endif()
if(KICKOS_QEMU_MACHINE STREQUAL "mps2-an385" AND NOT KICKOS_DIAG_TERSE)
  kickos_add_qemu_test(TARGET c SCRIPT c.sh)
endif()
set(_d OFF)
if(KICKOS_MEMORY_ENFORCED)
  set(_d ON)
endif()
if(_d AND KICKOS_QEMU_MPS2)
  kickos_add_qemu_test(TARGET d SCRIPT d.sh)
endif()
if(KICKOS_BOARD STREQUAL "microbit")
  add_test(NAME e COMMAND e.sh)
endif()
kickos_add_qemu_test(TARGET e SCRIPT e.sh)
'''


def main(argv):
    bad = problems('planted', PLANTED_BAD)
    if len(bad) != 4:
        print('FAIL: the planted board-name registrations read as %s, expected four refusals'
              % bad)
        return 1
    good = problems('planted', PLANTED_GOOD)
    if good:
        print('FAIL: a registration keyed on a fact is refused: %s' % good)
        return 1
    if len(argv) < 2:
        print('FAIL: no file to read, so the check would pass vacuously')
        return 1
    found = []
    calls = 0
    for path in argv[1:]:
        with open(path) as fh:
            text = fh.read()
        calls += sum(1 for _, st in statements(text) if CALL_RE.match(st))
        found += problems(path, text)
    for p in found:
        print('FAIL: %s' % p)
    if found:
        print('Key the registration on the fact its claim rests on (an arch, a Kconfig symbol, '
              'the machine, the image), never on a board name.')
        return 1
    if calls == 0:
        print('FAIL: %d file(s) hold no kickos_add_qemu_test call, so nothing was checked'
              % (len(argv) - 1))
        return 1
    print('PASS: %d emulator gate registration(s) in %d file(s), none under a board name'
          % (calls, len(argv) - 1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
