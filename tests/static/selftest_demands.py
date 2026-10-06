# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What each selftest arm asks of the board's fixed pools, read off the arm itself: the workers it
# asks pool_can_host for beside main, the objects it asks objects_can_host for out of main's task
# budgets, and the capabilities its own skip reason says it holds at once. Each ask holds what it
# probes, so pool_can_host also takes one semaphore and one capability slot for its gate, and
# objects_can_host one slot per object. tests/integration/gates/selftest.cmake expects an arm to
# skip wherever the posture provisions less than that, so the expectation and the arm's own
# decision rest on one number.
#
#   python3 tests/static/selftest_demands.py <selftest-source-dir> [--config <name>=<n>...]
#           [--undefined <name>...]
#   python3 tests/static/selftest_demands.py <selftest-source-dir> [--config <name>=<n>...]
#           [--undefined <name>...] --supply <pool>=<n>...
#   python3 tests/static/selftest_demands.py --self-test
#
# prints `<arm> <pool>=<n>...` per arm that states a demand, or with --supply the arms that ask
# any pool for more than it supplies, one per line. --supply names every pool in POOLS. --config
# gives the board configuration values a constant or a preprocessor guard of the selftest is
# stated from, and --undefined the macros the build leaves undefined.

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_seat_arms import spans  # noqa: E402
from check_starved_arms import TAP_ADD_RE, strip  # noqa: E402

POOLS = ('workers', 'caps', 'sems', 'mutexes', 'endpoints', 'notifies')
OBJECTS = ('sems', 'mutexes', 'endpoints', 'notifies')
WORKERS_RE = re.compile(r'\bpool_can_host\(\s*(?:static_cast<\s*int\s*>\(\s*([^()]+?)\s*\)|(\w+))\s*\)')
OBJECTS_RE = re.compile(r'\bobjects_can_host\(\s*\{([^{}]*)\}\s*\)')
FIELD_RE = re.compile(r'^\s*\.(\w+)\s*=\s*(\w+)\s*$')
CAPS_RE = re.compile(r'cap table too small \((\d+) concurrent caps\)')
CONST_RE = re.compile(r'\bconstexpr\s+(?:int|unsigned|uint32_t|size_t)\s+(\w+)\s*=\s*([\w\s+]+?)\s*;')
LITERAL_RE = re.compile(r'^(\d+)[uU]?$')
COND_RE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')
TERM_RE = re.compile(r'^\(?\s*(\w+)\s*(>=|<=|==|!=|>|<)\s*(\d+)[uU]?\s*\)?$')
DEFINED_RE = re.compile(r'^(not\s+|!\s*)?defined\s*(?:\(\s*(\w+)\s*\)|\s(\w+))$')
BARE_RE = re.compile(r'^(not\s+|!\s*)?(\w+)$')
AND_RE = re.compile(r'&&|\band\b')
UNDEFINED = None
OPS = {'>=': lambda a, b: a >= b, '<=': lambda a, b: a <= b, '==': lambda a, b: a == b,
       '!=': lambda a, b: a != b, '>': lambda a, b: a > b, '<': lambda a, b: a < b}


def term(t, config):
    """True or False where one term of a conjunction is decided by --config values alone, else
    None."""
    m = TERM_RE.match(t)
    if m:
        if m.group(1) not in config:
            return None
        n = config[m.group(1)]
        if n is UNDEFINED:
            n = 0
        return OPS[m.group(2)](n, int(m.group(3)))
    m = DEFINED_RE.match(t)
    val = None
    if m:
        name = m.group(2) or m.group(3)
        if name in config:
            val = config[name] is not UNDEFINED
    else:
        m = BARE_RE.match(t)
        if m and m.group(2) in config:
            val = config[m.group(2)] not in (UNDEFINED, 0)
    if val is not None and m.group(1):
        val = not val
    return val


def condition(expr, config):
    """True or False where <expr> is decided by --config values alone, else None. A conjunction
    holding one false term is false."""
    known = [term(t.strip(), config) for t in AND_RE.split(expr)]
    if False in known:
        return False
    if None in known:
        return None
    return True


def live(text, config):
    """<text> with every line of a preprocessor branch --config rules out blanked."""
    out = []
    stack = []
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        m = COND_RE.match(line)
        if m:
            kind = m.group(1)
            expr = m.group(2).split('//')[0]
            while expr.rstrip().endswith('\\') and i + 1 < len(lines):
                out.append(line)
                i += 1
                line = lines[i]
                expr = expr.rstrip()[:-1] + ' ' + line.split('//')[0]
            if kind in ('if', 'ifdef', 'ifndef'):
                val = None
                if kind == 'if':
                    val = condition(expr, config)
                elif kind == 'ifdef':
                    val = condition('defined(%s)' % expr.strip(), config)
                else:
                    val = condition('not defined(%s)' % expr.strip(), config)
                stack.append(val)
            elif kind == 'elif' and stack:
                if stack[-1] is True:
                    stack[-1] = False
                else:
                    stack[-1] = None
            elif kind == 'else' and stack:
                if stack[-1] is not None:
                    stack[-1] = not stack[-1]
            elif kind == 'endif' and stack:
                stack.pop()
            out.append(line)
            i += 1
            continue
        if False in stack:
            out.append('')
        else:
            out.append(line)
        i += 1
    return '\n'.join(out)


def value(expr, consts, what, seen=()):
    """A sum of literals, constexprs of the selftest and --config values."""
    total = 0
    for term in expr.split('+'):
        term = term.strip()
        lit = LITERAL_RE.match(term)
        if lit:
            total += int(lit.group(1))
        elif term in consts and term not in seen:
            total += value(consts[term], consts, what, seen + (term,))
        else:
            raise ValueError('%s names %s, which is neither a literal, a constexpr of the selftest '
                             'nor a --config value' % (what, term))
    return total


def own_demand(body, raw, consts):
    """{pool: n} that one function body states."""
    need = dict((p, 0) for p in POOLS)
    for cast, bare in WORKERS_RE.findall(body):
        need['workers'] = max(need['workers'], value(cast or bare, consts, 'a pool_can_host call'))
        need['sems'] = max(need['sems'], 1)
        need['caps'] = max(need['caps'], 1)
    for fields in OBJECTS_RE.findall(body):
        ask = dict((p, 0) for p in OBJECTS)
        for field in fields.split(','):
            if not field.strip():
                continue
            m = FIELD_RE.match(field)
            if not m or m.group(1) not in OBJECTS:
                raise ValueError('objects_can_host states `%s`, which is not `.<pool> = <n>` '
                                 'over %s' % (field.strip(), ', '.join(OBJECTS)))
            ask[m.group(1)] = value(m.group(2), consts, 'an objects_can_host field')
        for p in OBJECTS:
            need[p] = max(need[p], ask[p])
        need['caps'] = max(need['caps'], sum(ask.values()))
    for c in CAPS_RE.findall(raw):
        need['caps'] = max(need['caps'], int(c))
    return need


def demands(texts, config=None):
    """{arm: {pool: n}}, each the largest the arm's own body or a helper it calls states."""
    consts = dict((name, str(n)) for name, n in (config or {}).items() if n is not UNDEFINED)
    registered = {}
    own = {}
    calls = {}
    for text in texts:
        for name, v in CONST_RE.findall(text):
            consts[name] = v
        for arm, fn in TAP_ADD_RE.findall(live(text, config or {})):
            registered.setdefault(fn, []).append(arm)
    for text in texts:
        code = strip(text)
        lines = text.split('\n')
        code_lines = code.split('\n')
        for fn, a, b in spans(code):
            body = '\n'.join(code_lines[a:b + 1])
            need = own_demand(body, '\n'.join(lines[a:b + 1]), consts)
            prev = own.get(fn, dict((p, 0) for p in POOLS))
            own[fn] = dict((p, max(prev[p], need[p])) for p in POOLS)
            calls.setdefault(fn, set()).update(re.findall(r'\b(\w+)\s*\(', body))
    out = {}
    for fn, arms in registered.items():
        seen = set()
        todo = [fn]
        need = dict((p, 0) for p in POOLS)
        while todo:
            f = todo.pop()
            if f in seen or f not in own:
                continue
            seen.add(f)
            need = dict((p, max(need[p], own[f][p])) for p in POOLS)
            todo.extend(calls[f] - seen)
        if any(need.values()):
            for arm in arms:
                out[arm] = need
    return out


def short(found, supply):
    """The arms of <found> that ask any pool for more than <supply> holds."""
    return sorted(arm for arm, need in found.items()
                  if any(need[p] > supply[p] for p in POOLS))


PLANTED = [
    'constexpr int TRIES = 5;\n'
    'void helper()\n{\n    if (not pool_can_host(TRIES)) { return; }\n}\n'
    'void t_a()\n{\n    helper();\n}\n'
    'void t_b()\n{\n    tap::skip("cap table too small (6 concurrent caps)");\n}\n'
    'void t_c()\n{\n    tap::skip("pool too small (%d)", 9);\n}\n'
    'void t_d()\n{\n    if (not objects_can_host({.sems = 2, .endpoints = 1})) { return; }\n}\n'
    'constexpr unsigned CROWD = KICKOS_KERNEL_CORES + 1u;\n'
    'void t_f()\n{\n    if (not pool_can_host(static_cast<int>(CROWD + 1))) { return; }\n}\n'
    'void t_g()\n{\n    if (not pool_can_host(2)) { return; }\n}\n'
    'void t_h()\n{\n    if (not pool_can_host(2)) { return; }\n}\n'
    'void reg()\n{\n    TAP_ADD("a", t_a);\n    TAP_ADD("b", t_b);\n'
    '    TAP_ADD("c", t_c);\n    TAP_ADD("d", t_d);\n'
    '#if defined(X) && KICKOS_KERNEL_CORES > 1\n    TAP_ADD("f", t_f);\n#endif\n'
    '#if defined(KICKOS_ENABLE_SELFTEST)\n    TAP_ADD("g", t_g);\n#endif\n'
    '#if KICKOS_KERNEL_CORES > 1 \\\n    and not KICKOS_HAVE_ASPACE\n'
    '    TAP_ADD("h", t_h);\n#endif\n'
    '}\n']
PLANTED_CONFIG = {'KICKOS_KERNEL_CORES': 2}
ROOMY = {'workers': 5, 'caps': 6, 'sems': 2, 'mutexes': 1, 'endpoints': 1, 'notifies': 1}


def ask(workers=0, caps=0, sems=0, endpoints=0):
    return {'workers': workers, 'caps': caps, 'sems': sems, 'mutexes': 0, 'endpoints': endpoints,
            'notifies': 0}


def self_test():
    got = demands(PLANTED, PLANTED_CONFIG)
    want = {'a': ask(workers=5, caps=1, sems=1), 'b': ask(caps=6),
            'd': ask(caps=3, sems=2, endpoints=1), 'f': ask(workers=4, caps=1, sems=1),
            'g': ask(workers=2, caps=1, sems=1), 'h': ask(workers=2, caps=1, sems=1)}
    if got != want:
        print('FAIL: planted arms read as %s, expected %s' % (got, want))
        return 1
    if short(got, ROOMY):
        print('FAIL: a posture supplying every demand expects %s to skip' % short(got, ROOMY))
        return 1
    # A semaphore budget one short of d's two, which pool_can_host's gate alone still fits.
    for pool, arms in (('sems', ['d']), ('caps', ['b']), ('endpoints', ['d']), ('workers', ['a'])):
        posture = dict(ROOMY)
        posture[pool] = ROOMY[pool] - 1
        if short(got, posture) != arms:
            print('FAIL: a posture one %s short expects %s to skip, not %s'
                  % (pool, short(got, posture), arms))
            return 1
    bad = ['void t_e()\n{\n    objects_can_host({.sems = n});\n}\n'
           'void reg()\n{\n    TAP_ADD("e", t_e);\n}\n']
    try:
        demands(bad)
    except ValueError:
        pass
    else:
        print('FAIL: an objects_can_host field naming a runtime value was read as a demand')
        return 1
    one = demands(PLANTED, {'KICKOS_KERNEL_CORES': 1})
    if 'f' in one:
        print('FAIL: an arm registered only above one core is expected of a one-core build')
        return 1
    for config, absent, present in (
            ({'KICKOS_ENABLE_SELFTEST': UNDEFINED}, 'g', None),
            ({'KICKOS_ENABLE_SELFTEST': 1}, None, 'g'),
            ({'KICKOS_HAVE_ASPACE': 1}, 'h', None),
            ({'KICKOS_HAVE_ASPACE': 0}, None, 'h')):
        config['KICKOS_KERNEL_CORES'] = 2
        got = demands(PLANTED, config)
        if absent in got or (present and present not in got):
            print('FAIL: under %s the planted arms read as %s' % (config, sorted(got)))
            return 1
    try:
        demands(PLANTED)
    except ValueError:
        pass
    else:
        print('FAIL: a constant stated from a board configuration value no --config gives was read')
        return 1
    print('PASS: a helper\'s pool_can_host, a named constant, a cast sum over a configured one, an '
          'object ask and a skip reason are each read, an arm the configuration does not register '
          'is expected nowhere, a guard on a defined, undefined or valued macro is followed across '
          'a continued line, and a posture one short of any pool expects the arm asking it to skip')
    return 0


def main(argv):
    if argv[1:] == ['--self-test']:
        return self_test()
    src = argv[1]
    rest = argv[2:]
    config = {}
    while rest[:1] in (['--config'], ['--undefined']) and len(rest) > 1:
        name, _, n = rest[1].partition('=')
        if rest[0] == '--undefined':
            if not re.match(r'^[A-Z_][A-Z0-9_]*$', rest[1]):
                print('FAIL: --undefined takes <name>, not %s' % rest[1], file=sys.stderr)
                return 1
            config[rest[1]] = UNDEFINED
        elif not re.match(r'^[A-Z_][A-Z0-9_]*$', name) or not re.match(r'^\d+$', n):
            print('FAIL: --config takes <name>=<n>, not %s' % rest[1], file=sys.stderr)
            return 1
        else:
            config[name] = int(n)
        rest = rest[2:]
    supply = None
    if rest:
        if rest[0] != '--supply':
            print('FAIL: usage: selftest_demands.py <dir> [--config <name>=<n>...] '
                  '[--undefined <name>...] [--supply <pool>=<n>...]', file=sys.stderr)
            return 1
        supply = {}
        for item in rest[1:]:
            pool, _, n = item.partition('=')
            if pool not in POOLS or not re.match(r'^-?\d+$', n):
                print('FAIL: --supply takes <pool>=<n> over %s, not %s' % (', '.join(POOLS), item),
                      file=sys.stderr)
                return 1
            supply[pool] = int(n)
        if sorted(supply) != sorted(POOLS):
            print('FAIL: --supply names %s; every one of %s is owed'
                  % (', '.join(sorted(supply)), ', '.join(POOLS)), file=sys.stderr)
            return 1
    texts = []
    for name in sorted(os.listdir(src)):
        if name.endswith('.cc'):
            with open(os.path.join(src, name)) as fh:
                texts.append(fh.read())
    try:
        found = demands(texts, config)
    except ValueError as e:
        print('FAIL: %s' % e, file=sys.stderr)
        return 1
    if not found:
        print('FAIL: no arm in %s states a demand, so every expectation would be empty' % src,
              file=sys.stderr)
        return 1
    if supply is not None:
        for arm in short(found, supply):
            print(arm)
        return 0
    for arm in sorted(found):
        print('%s %s' % (arm, ' '.join('%s=%d' % (p, found[arm][p]) for p in POOLS)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
