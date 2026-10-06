# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The selftest's reservation_refused_skips names, in its arms[], every registered arm that
# reaches st_ram_alloc or st_ram_alloc_as, each under the name TAP_ADD registers it by, and
# nothing else. Read off the selftest sources: a function reaches a call site when it holds
# one or names a function that does. Planted copies then add an uncovered arm and drop a
# covered one, and each must be refused.
#
#   python3 tests/static/check_starved_arms.py <selftest-source-dir>

import os
import re
import sys

SITE_RE = re.compile(r'\bst_ram_alloc(?:_as)?\s*\(')
ARM_ENTRY_RE = re.compile(r'\{\s*"(\w+)"\s*,\s*(\w+)\s*\}')
TAP_ADD_RE = re.compile(r'\bTAP_ADD\w*\(\s*"(\w+)"\s*,\s*(\w+)')
HOLDER = 't_reservation_refused_skips'


def strip(text):
    """The source with comments and literals blanked, line breaks kept."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            if j < 0:
                j = n
            i = j
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            if j < 0:
                j = n
            out.append('\n' * text.count('\n', i, j))
            i = j + 2
            continue
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == '\\':
                    j += 1
                j += 1
            out.append(c + c)
            i = j + 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def functions(code):
    """{name: body} of every function defined outside another body; namespaces are see-through."""
    found = {}
    depth = 0
    opaque = []
    i = 0
    n = len(code)
    start = None
    name = None
    while i < n:
        c = code[i]
        if c == '{':
            head = code[max(0, i - 300):i]
            if depth == 0:
                if re.search(r'\bnamespace\b[\w\s:]*$', head):
                    opaque.append(False)
                    i += 1
                    continue
                m = re.search(r'\b(\w+)\s*\([^(){};]*(?:\([^(){};]*\)[^(){};]*)*\)\s*(?:const\s*)?(?:noexcept\s*)?$',
                              head)
                if m and m.group(1) not in ('if', 'for', 'while', 'switch', 'return', 'sizeof'):
                    name = m.group(1)
                    start = i
                else:
                    name = None
                    start = i
            depth += 1
            opaque.append(True)
        elif c == '}':
            if opaque and not opaque[-1]:
                opaque.pop()
                i += 1
                continue
            if opaque:
                opaque.pop()
            depth -= 1
            if depth == 0 and name is not None:
                found.setdefault(name, '')
                found[name] += code[start:i + 1]
                name = None
        i += 1
    return found


def listed(raw):
    """[(name, fn)] of HOLDER's arms[], read off the unstripped source."""
    m = re.search(r'void %s\(\)\s*\{.*?\n    \}' % HOLDER, raw, re.S)
    if not m:
        return []
    return ARM_ENTRY_RE.findall(m.group(0))


def site_functions(bodies):
    return set(f for f, body in bodies.items() if SITE_RE.search(body) and not f.startswith('st_ram_alloc'))


def problems(texts):
    """What is wrong with the sources <texts>, as one line each; empty when nothing is."""
    code = '\n'.join(strip(t) for t in texts)
    raw = '\n'.join(texts)
    bodies = functions(code)
    registered = dict((fn, arm) for arm, fn in TAP_ADD_RE.findall(raw))
    if HOLDER not in bodies:
        return ["no %s in the selftest sources" % HOLDER]
    entries = listed(raw)
    if not entries:
        return ["%s lists no arm" % HOLDER]
    out = []
    covered = set()
    for arm, fn in entries:
        if registered.get(fn) != arm:
            out.append("%s lists %s as \"%s\", which TAP_ADD registers as \"%s\""
                       % (HOLDER, fn, arm, registered.get(fn)))
        covered.add(fn)
    sites = site_functions(bodies)
    if not sites:
        return ["no function calls st_ram_alloc, so nothing was checked"]
    callers = {}
    for f, body in bodies.items():
        if f == HOLDER:
            continue
        for g in set(re.findall(r'\w+', body)):
            if g != f and g in bodies:
                callers.setdefault(g, set()).add(f)
    reaching = set()
    todo = list(sites)
    while todo:
        f = todo.pop()
        if f in reaching:
            continue
        reaching.add(f)
        todo.extend(callers.get(f, ()))
    for fn in sorted(reaching):
        if fn in registered and fn not in covered:
            out.append("the arm \"%s\" (%s) reaches st_ram_alloc and %s does not list it"
                       % (registered[fn], fn, HOLDER))
    for arm, fn in entries:
        if fn not in reaching:
            out.append("%s lists \"%s\" (%s), which reaches no st_ram_alloc" % (HOLDER, arm, fn))
    return out


def main():
    src = sys.argv[1]
    names = sorted(f for f in os.listdir(src) if f.endswith('.cc'))
    texts = []
    for f in names:
        with open(os.path.join(src, f)) as fh:
            texts.append(fh.read())
    found = problems(texts)
    for line in found:
        print("FAIL: %s" % line)
    if found:
        return 1

    main_at = names.index('main.cc')
    base = texts[main_at]
    added = list(texts)
    added[main_at] = base + ('\nvoid t_planted_reserver()\n{\n    (void)st_ram_alloc(64);\n}\n'
                             'void planted_register()\n{\n    TAP_ADD("planted_reserver", t_planted_reserver);\n}\n')
    if not any('planted_reserver' in p for p in problems(added)):
        print("FAIL: a planted arm reaching st_ram_alloc that %s does not list is not refused" % HOLDER)
        return 1
    first = ARM_ENTRY_RE.search(base[base.index('void %s()' % HOLDER):])
    dropped = list(texts)
    dropped[main_at] = base.replace(first.group(0) + ',', '', 1)
    if dropped[main_at] == base or not any('"%s"' % first.group(1) in p for p in problems(dropped)):
        print("FAIL: dropping \"%s\" from %s's arms is not refused" % (first.group(1), HOLDER))
        return 1
    code = '\n'.join(strip(t) for t in texts)
    print("PASS: %s lists the %d arm(s) reaching st_ram_alloc from %d function(s) calling it"
          % (HOLDER, len(listed('\n'.join(texts))), len(site_functions(functions(code)))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
