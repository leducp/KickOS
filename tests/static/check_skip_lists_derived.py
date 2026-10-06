# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No expected-skip or expected-partial set names an arm under a board predicate. Such a list is a
# measurement of one board taken once, and it goes stale the moment an arm or a provisioning
# changes; every expectation is derived from the facts that decide it instead. A set that only
# carries a derived variable under a board predicate is not a list and passes.
#
#   python3 tests/static/check_skip_lists_derived.py <cmake file>...

import re
import sys

SET_RE = re.compile(r'^\s*(?:set|list\s*\(\s*(?:APPEND|PREPEND|INSERT))\s*\(?\s*'
                    r'(KICKOS_EXPECT_(?:SKIPS|PARTIALS)|\w*_(?:skips|partials))\b(.*)$', re.I)
ENV_RE = re.compile(r'"EXPECT_(?:SKIPS|PARTIALS)=([^"]*)"')
OPEN_RE = re.compile(r'^\s*if\s*\((.*)$', re.I)
ELSE_RE = re.compile(r'^\s*(elseif|else)\s*\((.*)$', re.I)
CLOSE_RE = re.compile(r'^\s*endif\s*\(', re.I)
BOARD = 'KICKOS_BOARD'


def literal_names(values):
    """The bare words in <values> once every ${...} reference is taken out."""
    rest = re.sub(r'\$\{[^}]*\}', ' ', values)
    rest = rest.replace(')', ' ')
    return [w for w in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', rest)
            if w not in ('APPEND', 'PREPEND', 'INSERT', 'PARENT_SCOPE')]


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


def problems(path, text):
    out = []
    stack = []        # per open if: every condition its chain has stated
    for n, st in statements(text):
        m = OPEN_RE.match(st)
        if m:
            stack.append(m.group(1))
            continue
        m = ELSE_RE.match(st)
        if m and stack:
            stack[-1] += ' ' + m.group(2)
            continue
        if CLOSE_RE.match(st) and stack:
            stack.pop()
            continue
        if not any(BOARD in c for c in stack):
            continue
        names = []
        m = SET_RE.match(st)
        if m:
            names = literal_names(m.group(2))
        for v in ENV_RE.findall(st):
            names += literal_names(v)
        if names:
            out.append('%s:%d: names %s under a %s predicate' % (path, n, ' '.join(names), BOARD))
    return out


PLANTED_BAD = '''if(KICKOS_BOARD STREQUAL "f302nucleo" AND KICKOS_ENABLE_SELFTEST)
  set(_f3_skips
      mutex_basic prio_self_raise_lower)
endif()
if(KICKOS_BOARD STREQUAL "microbit")
  list(APPEND KICKOS_EXPECT_PARTIALS caller_stack)
  set_property(TEST t APPEND PROPERTY ENVIRONMENT "EXPECT_SKIPS=uart_service")
endif()
'''
PLANTED_GOOD = '''if(KICKOS_MAX_THREADS LESS 5)
  list(APPEND KICKOS_EXPECT_SKIPS task_dead_after_every_sweep)
endif()
if(KICKOS_BOARD STREQUAL "microbit")
  foreach(_img IN LISTS _images)
    set_property(TEST microbit_${_img} APPEND PROPERTY ENVIRONMENT
      "EXPECT_SKIPS=${_mb_img_skips}" "EXPECT_PARTIALS=${_mb_img_partials}")
  endforeach()
endif()
'''


def main(argv):
    bad = problems('planted', PLANTED_BAD)
    if len(bad) != 3:
        print('FAIL: the planted board lists read as %s, expected three refusals' % bad)
        return 1
    if problems('planted', PLANTED_GOOD):
        print('FAIL: a fact-keyed rule or a derived set under a board predicate is refused')
        return 1
    found = []
    for path in argv[1:]:
        with open(path) as fh:
            found += problems(path, fh.read())
    for p in found:
        print('FAIL: %s' % p)
    if found:
        print('Derive the expectation from the facts that decide it '
              '(tests/integration/gates/selftest.cmake).')
        return 1
    if len(argv) < 2:
        print('FAIL: no file to read, so the check would pass vacuously')
        return 1
    print('PASS: %d file(s): no expected-skip or expected-partial set names an arm under a board '
          'predicate' % (len(argv) - 1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
