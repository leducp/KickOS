# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The selftest arms that skip because their doorbell keeps no seat, read off the selftest
# sources by that skip's reason, are exactly the ones tests/integration/gates/selftest.cmake
# expects to skip where the chip file states no doorbell_seat, each under the name TAP_ADD
# registers it by. Planted copies then add an unlisted seat skip and drop a listed one, and each
# must be refused.
#
#   python3 tests/static/check_seat_arms.py <selftest-source-dir> <selftest.cmake>

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_starved_arms import TAP_ADD_RE, strip  # noqa: E402

REASON = 'tap::skip("this doorbell has no seat'
LIST_RE = re.compile(r'^set\(_selftest_seat_skips\s+([^)]*)\)', re.M)
RULE_RE = re.compile(r'if\(NOT KICKOS_CHIP_DOORBELL_SEAT\)\s*\n\s*list\(APPEND KICKOS_EXPECT_SKIPS '
                     r'\$\{_selftest_seat_skips\}\)')


def spans(code):
    """[(name, first line, last line)] of every function defined outside another body."""
    found = []
    depth = 0
    opaque = []
    name = None
    start = 0
    for i, c in enumerate(code):
        if c == '{':
            head = code[max(0, i - 300):i]
            if depth == 0:
                if re.search(r'\bnamespace\b[\w\s:]*$', head):
                    opaque.append(False)
                    continue
                m = re.search(r'\b(\w+)\s*\([^(){};]*(?:\([^(){};]*\)[^(){};]*)*\)\s*(?:const\s*)?'
                              r'(?:noexcept\s*)?$', head)
                name = None
                if m and m.group(1) not in ('if', 'for', 'while', 'switch', 'return', 'sizeof'):
                    name = m.group(1)
                start = i
            depth += 1
            opaque.append(True)
        elif c == '}':
            if opaque and not opaque[-1]:
                opaque.pop()
                continue
            if opaque:
                opaque.pop()
            depth -= 1
            if depth == 0 and name is not None:
                found.append((name, code.count('\n', 0, start), code.count('\n', 0, i)))
                name = None
    return found


def seat_arms(texts):
    """({arm names}, [problems]) of the seat skips in <texts>."""
    arms = set()
    out = []
    registered = {}
    for text in texts:
        for arm, fn in TAP_ADD_RE.findall(text):
            registered[fn] = arm
    for text in texts:
        fns = spans(strip(text))
        for n, line in enumerate(text.split('\n')):
            if REASON not in line:
                continue
            owner = [f for f, a, b in fns if a <= n <= b]
            if not owner:
                out.append("a seat skip at line %d sits in no function" % (n + 1))
                continue
            if owner[0] not in registered:
                out.append("%s skips on a seatless doorbell and is no registered arm" % owner[0])
                continue
            arms.add(registered[owner[0]])
    return arms, out


def problems(texts, cmake):
    arms, out = seat_arms(texts)
    if not arms and not out:
        out.append("no arm in the selftest sources skips on a seatless doorbell, so the check reads nothing")
    m = LIST_RE.search(cmake)
    if not m:
        return out + ["selftest.cmake sets no _selftest_seat_skips"]
    if not RULE_RE.search(cmake):
        out.append("selftest.cmake does not expect _selftest_seat_skips where KICKOS_CHIP_DOORBELL_SEAT is OFF")
    listed = set(m.group(1).split())
    for arm in sorted(arms - listed):
        out.append("the arm \"%s\" skips on a seatless doorbell and _selftest_seat_skips does not list it" % arm)
    for arm in sorted(listed - arms):
        out.append("_selftest_seat_skips lists \"%s\", which skips on no seatless doorbell" % arm)
    return out


def main():
    src, cmake_path = sys.argv[1], sys.argv[2]
    names = sorted(f for f in os.listdir(src) if f.endswith('.cc'))
    texts = []
    for f in names:
        with open(os.path.join(src, f)) as fh:
            texts.append(fh.read())
    with open(cmake_path) as fh:
        cmake = fh.read()
    found = problems(texts, cmake)
    for line in found:
        print("FAIL: %s" % line)
    if found:
        return 1

    added = texts + ['void t_planted_seat()\n{\n    %s, so planted");\n}\n'
                     'void planted_register()\n{\n    TAP_ADD("planted_seat", t_planted_seat);\n}\n' % REASON]
    if not any('planted_seat' in p for p in problems(added, cmake)):
        print("FAIL: a planted seat skip that _selftest_seat_skips does not list is not refused")
        return 1
    listed = LIST_RE.search(cmake).group(1).split()
    dropped = cmake.replace(LIST_RE.search(cmake).group(0),
                            'set(_selftest_seat_skips %s)' % ' '.join(listed[1:]), 1)
    if not any('"%s"' % listed[0] in p for p in problems(texts, dropped)):
        print("FAIL: dropping \"%s\" from _selftest_seat_skips is not refused" % listed[0])
        return 1
    unruled = cmake.replace('if(NOT KICKOS_CHIP_DOORBELL_SEAT)', 'if(FALSE)', 1)
    if not problems(texts, unruled):
        print("FAIL: a _selftest_seat_skips no seatless chip expects is not refused")
        return 1
    print("PASS: _selftest_seat_skips lists the %d arm(s) that skip on a seatless doorbell: %s"
          % (len(listed), ' '.join(sorted(listed))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
