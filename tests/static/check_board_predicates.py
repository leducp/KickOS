# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Nothing a gate fragment or the app selection states may rest on a board's NAME, because a
# board name in that place is a list of where something was once run or measured, and the next
# preset is left out in silence. Three statements are refused under a condition that names a
# board, after an earlier such condition returned out of the file, or under a condition reading
# a variable set under one:
#
#   an emulator gate   kickos_add_qemu_test is total over the fleet, so the only condition a
#                      call owes is the fact its claim rests on: an arch, a Kconfig symbol, the
#                      emulated machine, the existence of an image.
#   an expected arm    an expected-skip or expected-partial set naming an arm is a measurement
#                      of one board taken once; it is derived from the facts that decide it
#                      instead. A set carrying only a derived variable is not a list and passes.
#   an app selection   add_subdirectory or kickos_add_diagnostic_apps, keyed on what the app
#                      needs instead. A file the app ships for the board, which an EXISTS or
#                      IS_DIRECTORY operand asks after, is such a fact.
#
#   python3 tests/static/check_board_predicates.py <cmake file>...

import re
import sys

OPEN_RE = re.compile(r'^\s*if\s*\((.*)$', re.I)
ELSE_RE = re.compile(r'^\s*(?:elseif|else)\s*\((.*)$', re.I)
CLOSE_RE = re.compile(r'^\s*endif\s*\(', re.I)
RETURN_RE = re.compile(r'^\s*return\s*\(', re.I)
SET_RE = re.compile(r'^\s*(?:set|list\s*\(\s*(?:APPEND|PREPEND|INSERT))\s*\(?\s*([A-Za-z_][A-Za-z0-9_]*)(.*)$',
                    re.I)
CALL_RE = re.compile(r'^\s*kickos_add_qemu_test\s*\(', re.I)
SELECT_RE = re.compile(r'^\s*(?:add_subdirectory|kickos_add_diagnostic_apps)\s*\(', re.I)
FILE_TEST_RE = re.compile(r'\b(?:EXISTS|IS_DIRECTORY)\s+(?:"[^"]*"|\S+)')
EXPECT_RE = re.compile(r'^(KICKOS_EXPECT_(?:SKIPS|PARTIALS)|\w*_(?:skips|partials))$', re.I)
ENV_RE = re.compile(r'"EXPECT_(?:SKIPS|PARTIALS)=([^"]*)"')
# The board's name, every name built on it but the newlib flavour, which is a board fact, and the
# CTest prefix the integration directory spells out of it.
BOARD_RE = re.compile(r'\b(KICKOS_BOARD(?!_NEWLIB\b)\w*|_tag)\b')


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


def literal_names(values):
    """The bare words in <values> once every ${...} reference is taken out."""
    rest = re.sub(r'\$\{[^}]*\}', ' ', values)
    rest = rest.replace(')', ' ')
    return [w for w in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', rest)
            if w not in ('APPEND', 'PREPEND', 'INSERT', 'PARENT_SCOPE')]


def reads_board(cond, tainted):
    if BOARD_RE.search(cond):
        return True
    return any(word in tainted for word in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', cond))


def names_board(cond, tainted):
    """Whether <cond> names a board, read whole and, for an app selection, without its file
    tests."""
    return (reads_board(cond, tainted), reads_board(FILE_TEST_RE.sub(' ', cond), tainted))


def problems(path, text):
    out = []
    stack = []        # per open if: does any condition of its chain name a board, as names_board
    tainted = set()
    guarded = [0, 0]  # line of a board-conditioned return() already passed, or 0, as names_board
    for n, st in statements(text):
        m = OPEN_RE.match(st)
        if m:
            stack.append(names_board(m.group(1), tainted))
            continue
        m = ELSE_RE.match(st)
        if m and stack:
            stack[-1] = tuple(a or b for a, b in zip(stack[-1], names_board(m.group(1), tainted)))
            continue
        if CLOSE_RE.match(st) and stack:
            stack.pop()
            continue
        under = [any(s[i] for s in stack) for i in (0, 1)]
        if RETURN_RE.match(st) and any(under):
            guarded = [g or (u and n) for g, u in zip(guarded, under)]
            continue
        where = ['', '']
        for i in (0, 1):
            if under[i]:
                where[i] = 'under a board name'
            elif guarded[i]:
                where[i] = 'past a board-name return at line %d' % guarded[i]
        names = []
        m = SET_RE.match(st)
        if m and EXPECT_RE.match(m.group(1)):
            names = literal_names(m.group(2))
        for v in ENV_RE.findall(st):
            names += literal_names(v)
        if names and where[0]:
            out.append('%s:%d: an expected-arm set names %s %s'
                       % (path, n, ' '.join(names), where[0]))
        if m and (under[0] or BOARD_RE.search(m.group(2))):
            tainted.add(m.group(1))
            continue
        if CALL_RE.match(st) and where[0]:
            out.append('%s:%d: an emulator gate registered %s' % (path, n, where[0]))
        if SELECT_RE.match(st) and where[1]:
            out.append('%s:%d: an app selected %s' % (path, n, where[1]))
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
if(KICKOS_BOARD STREQUAL "f302nucleo" AND KICKOS_ENABLE_SELFTEST)
  set(_f3_skips
      mutex_basic prio_self_raise_lower)
endif()
if(KICKOS_BOARD STREQUAL "microbit")
  list(APPEND KICKOS_EXPECT_PARTIALS caller_stack)
  set_property(TEST t APPEND PROPERTY ENVIRONMENT "EXPECT_SKIPS=uart_service")
endif()
if(KICKOS_BOARD STREQUAL "frdmk64f"
   OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gpioblink/systems/${KICKOS_BOARD}.yaml")
  add_subdirectory(gpioblink)
endif()
if(_c)
  kickos_add_diagnostic_apps(fault)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_BOARD}.cmake")
  kickos_add_qemu_test(TARGET f SCRIPT f.sh)
endif()
if(KICKOS_BOARD_INCLUDE_DIR MATCHES "f302")
  list(APPEND KICKOS_EXPECT_SKIPS mutex_basic)
endif()
if(NOT _tag STREQUAL "microbit")
  return()
endif()
kickos_add_qemu_test(TARGET d SCRIPT d.sh)
list(APPEND KICKOS_EXPECT_SKIPS uart_service)
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
if(KICKOS_MAX_THREADS LESS 5 OR KICKOS_BOARD_NEWLIB STREQUAL "nano")
  list(APPEND KICKOS_EXPECT_SKIPS task_dead_after_every_sweep)
endif()
if(KICKOS_BOARD STREQUAL "microbit")
  add_test(NAME e COMMAND e.sh)
  foreach(_img IN LISTS _images)
    set_property(TEST microbit_${_img} APPEND PROPERTY ENVIRONMENT
      "EXPECT_SKIPS=${_mb_img_skips}" "EXPECT_PARTIALS=${_mb_img_partials}")
  endforeach()
endif()
kickos_add_qemu_test(TARGET e SCRIPT e.sh)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gpioblink/systems/${KICKOS_BOARD}.yaml"
   AND NOT KICKOS_AMP_NODE)
  add_subdirectory(gpioblink)
endif()
if(KICKOS_MEMORY_ENFORCED)
  kickos_add_diagnostic_apps(specfault)
endif()
'''


def main(argv):
    bad = problems('planted', PLANTED_BAD)
    if len(bad) != 12:
        print('FAIL: the planted board-name statements read as %s, expected twelve refusals' % bad)
        return 1
    good = problems('planted', PLANTED_GOOD)
    if good:
        print('FAIL: a statement keyed on a fact is refused: %s' % good)
        return 1
    if len(argv) < 2:
        print('FAIL: no file to read, so the check would pass vacuously')
        return 1
    found = []
    calls = 0
    selections = 0
    for path in argv[1:]:
        with open(path) as fh:
            text = fh.read()
        calls += sum(1 for _, st in statements(text) if CALL_RE.match(st))
        selections += sum(1 for _, st in statements(text) if SELECT_RE.match(st))
        found += problems(path, text)
    for p in found:
        print('FAIL: %s' % p)
    if found:
        print('Key the statement on the fact it rests on (an arch, a Kconfig symbol, the machine, '
              'the image, a file the app ships), never on a board name; derive an expectation from '
              'the facts that decide it (tests/integration/gates/selftest.cmake).')
        return 1
    if calls == 0 or selections == 0:
        print('FAIL: %d file(s) hold %d kickos_add_qemu_test call(s) and %d app selection(s), so '
              'one corpus was not read' % (len(argv) - 1, calls, selections))
        return 1
    print('PASS: %d file(s), %d emulator gate registration(s) and %d app selection(s): none and no '
          'expected-arm set rests on a board name' % (len(argv) - 1, calls, selections))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
