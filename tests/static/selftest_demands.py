# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What each selftest arm asks of the board's fixed pools, read off the arm itself: the workers
# beside main, the tasks, objects, capability slots and IRQ line its TAP_ASK or TAP_ASK_LINE
# states. Each ask holds what it probes, so a worker probe also takes one semaphore and one
# capability slot for its gate, and the ask one slot per object and one for the line.
# tests/integration/gates/selftest.cmake expects an arm to skip wherever the posture provisions
# less than that, so the expectation and the arm's own decision rest on one number.
#
#   python3 tests/static/selftest_demands.py <selftest-source-dir> [--config <name>[=<n>]...]
#           [--undefined <name>...]
#   python3 tests/static/selftest_demands.py <selftest-source-dir> [--config <name>[=<n>]...]
#           [--undefined <name>...] --supply <pool>=<n>...
#   python3 tests/static/selftest_demands.py <selftest-source-dir> [--config <name>[=<n>]...]
#           [--undefined <name>...] --count
#   python3 tests/static/selftest_demands.py --self-test
#
# prints `<arm> <pool>=<n>...` per arm that states a demand, with --supply the arms that ask any
# pool for more than it supplies, or with --count the arms each region of main.cc registers, one per
# line. --supply names every pool in POOLS. --config gives the board configuration values a constant
# or a preprocessor guard of the selftest is stated from, and --undefined the macros the build
# leaves undefined. --count refuses a guard those do not decide.

import os
import re
import sys

TAP_ADD_RE = re.compile(r'\bTAP_ADD\w*\(\s*"(\w+)"\s*,\s*(\w+)')
POOLS = ('workers', 'tasks', 'irqs', 'caps', 'sems', 'mutexes', 'endpoints', 'notifies')
OBJECTS = ('sems', 'mutexes', 'endpoints', 'notifies')
FIELDS = ('workers', 'tasks') + OBJECTS + ('caps', 'irqs', 'irq_line')
ASK_RE = re.compile(r'\bTAP_ASK(_LINE)?\s*\(')
FIELD_RE = re.compile(r'^\s*\.(\w+)\s*=\s*(\w+)\s*$')
LINE_RE = re.compile(r'^\s*\.irq_line\s*=\s*[\w\s+]+$')
CONST_RE = re.compile(r'\bconstexpr\s+(?:int|unsigned|uint32_t|size_t)\s+(\w+)\s*=\s*([\w\s+]+?)\s*;')
LITERAL_RE = re.compile(r'^(\d+)[uU]?$')
COND_RE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')
TOKEN_RE = re.compile(r'\s*(?:(0[xX][0-9a-fA-F]+|[1-9]\d*|0)[uUlL]*(?![\w.])|([A-Za-z_]\w*)'
                      r'|(&&|\|\||==|!=|>=|<=|[()!<>]))')
KEYWORDS = ('and', 'or', 'not')
VALUE_RE = re.compile(r'^(0[xX][0-9a-fA-F]+|[1-9]\d*|0)$')
FIRST_RE = re.compile(r'^[ \t]*#[ \t]*if[ \t]+KICKOS_SELFTEST_REGION\b', re.M)
SELECTOR_RE = re.compile(r'^[ \t]*#[ \t]*if[ \t]+KICKOS_SELFTEST_REGION\(\d+\)[ \t]*\n'
                         r'[ \t]*#[ \t]*define[ \t]+TAP_ADD\(.*\n[ \t]*#[ \t]*else[ \t]*\n'
                         r'[ \t]*#[ \t]*define[ \t]+TAP_ADD\(.*\n[ \t]*#[ \t]*endif[ \t]*$', re.M)
BRANCH_RE = re.compile(r'^[ \t]*#[ \t]*(elif|else)\b', re.M)
REG_RE = re.compile(r'^\s*TAP_ADD(?:_IRQ|_PINNED)?\(\s*""\s*(?:,\s*\w+\s*)+\)\s*;\s*$')
DEFINE_RE = re.compile(r'^\s*#\s*define\b')
UNDEF_RE = re.compile(r'^\s*#\s*undef\s+TAP_ADD\s*$')
STRAY_RE = re.compile(r'\bTAP_ADD|\btap\s*::\s*add\b')
REGION = '@region'
UNDEFINED = None
OPS = {'>=': lambda a, b: a >= b, '<=': lambda a, b: a <= b, '==': lambda a, b: a == b,
       '!=': lambda a, b: a != b, '>': lambda a, b: a > b, '<': lambda a, b: a < b}


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


class Undecided(Exception):
    """A guard this reader cannot parse."""


def tokens(expr):
    out = []
    pos = 0
    while True:
        m = TOKEN_RE.match(expr, pos)
        if m is None:
            if expr[pos:].strip():
                raise Undecided()
            return out
        if m.group(1):
            out.append(('num', int(m.group(1), 0)))
        elif m.group(2):
            out.append(('id', m.group(2)))
        else:
            out.append(('op', m.group(3)))
        pos = m.end()


def evaluate(expr, config):
    """<expr> as the preprocessor would read it under <config>: an int, or None where a value it
    reads is not in <config>. A name in <config> as UNDEFINED reads 0, as the preprocessor reads an
    undefined one."""
    toks = tokens(expr)
    at = [0]

    def peek():
        if at[0] < len(toks):
            return toks[at[0]]
        return (None, None)

    def take(kind=None, text=None):
        tok = peek()
        if tok[0] is None or (kind and tok[0] != kind) or (text and tok[1] != text):
            raise Undecided()
        at[0] += 1
        return tok

    def accept(*texts):
        tok = peek()
        if tok[0] in ('op', 'id') and tok[1] in texts:
            at[0] += 1
            return True
        return False

    def primary():
        if accept('('):
            v = disjunction()
            take('op', ')')
            return v
        kind, text = take()
        if kind == 'num':
            return text
        if kind != 'id' or text in KEYWORDS:
            raise Undecided()
        if text == 'defined':
            paren = accept('(')
            name = take('id')[1]
            if paren:
                take('op', ')')
            if name not in config:
                return None
            return int(config[name] is not UNDEFINED)
        if peek() == ('op', '('):
            raise Undecided()
        if text not in config:
            return None
        if config[text] is UNDEFINED:
            return 0
        return config[text]

    def unary():
        if accept('!', 'not'):
            v = unary()
            if v is None:
                return None
            return int(not v)
        return primary()

    def comparison():
        v = unary()
        tok = peek()
        if tok[0] == 'op' and tok[1] in OPS:
            at[0] += 1
            w = unary()
            if v is None or w is None:
                return None
            return int(OPS[tok[1]](v, w))
        return v

    def conjunction():
        vals = [comparison()]
        while accept('&&', 'and'):
            vals.append(comparison())
        if len(vals) == 1:
            return vals[0]
        if 0 in vals:
            return 0
        if None in vals:
            return None
        return 1

    def disjunction():
        vals = [conjunction()]
        while accept('||', 'or'):
            vals.append(conjunction())
        if len(vals) == 1:
            return vals[0]
        if None in vals:
            return None
        return int(any(vals))

    v = disjunction()
    if at[0] != len(toks):
        raise Undecided()
    return v


def condition(expr, config):
    """True or False where <expr> is decided by --config values alone, else None."""
    try:
        v = evaluate(expr, config)
    except Undecided:
        return None
    if v is None:
        return None
    return v != 0


def live(text, config, strict=None):
    """<text> with every line of a preprocessor branch --config rules out blanked. Given the file's
    name as strict, refuses an #if --config does not decide inside no branch it rules out."""
    out = []
    stack = []
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        m = COND_RE.match(line)
        if m:
            kind = m.group(1)
            first = i
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
                if strict and val is None and False not in stack:
                    raise ValueError('%s line %d: the configuration does not decide `%s`'
                                     % (strict, first + 1, ' '.join(expr.split())))
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


def arm_counts(texts, config):
    """[arms] per region of main.cc under <config>, from <texts>, each source file's text by name.
    Raises ValueError where a registration cannot be counted exactly."""
    for name, text in sorted(texts.items()):
        for line in strip(text).split('\n'):
            if name != 'main.cc' and STRAY_RE.search(line) and not DEFINE_RE.match(line):
                raise ValueError('%s registers an arm, which only main.cc\'s regions may' % name)
    raw = texts['main.cc'].split('\n')
    code = strip(texts['main.cc'])
    m = FIRST_RE.search(code)
    if not m:
        raise ValueError('main.cc cuts no region')
    tail = SELECTOR_RE.sub(lambda s: REGION + '\n' * s.group(0).count('\n'), code[m.start():])
    if BRANCH_RE.search(tail):
        raise ValueError('main.cc registers an arm under an #elif or #else, which --count does not '
                         'read; state each guard on its own #if')
    counts = []
    open_region = False
    lines = live('\n' * code.count('\n', 0, m.start()) + tail, config, 'main.cc').split('\n')
    for i, line in enumerate(lines):
        if line == REGION:
            counts.append(0)
            open_region = True
        elif UNDEF_RE.match(line):
            open_region = False
        elif REG_RE.match(line):
            if not open_region:
                raise ValueError('main.cc line %d registers %s outside every region'
                                 % (i + 1, TAP_ADD_RE.search(raw[i]).group(1)))
            counts[-1] += 1
        elif STRAY_RE.search(line):
            raise ValueError('main.cc line %d is not one registration --count can read: %s'
                             % (i + 1, raw[i].strip()))
    return counts


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


def asks(body):
    """[[field]] of every TAP_ASK and TAP_ASK_LINE in <body>, the line argument dropped."""
    found = []
    for m in ASK_RE.finditer(body):
        args = ['']
        depth = 0
        for c in body[m.end():]:
            if c == ')' and depth == 0:
                break
            if c == ',' and depth == 0:
                args.append('')
                continue
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            args[-1] += c
        if m.group(1):
            args = args[1:]
        found.append(args)
    return found


def own_demand(body, consts):
    """{pool: n} that one function body states."""
    need = dict((p, 0) for p in POOLS)
    for fields in asks(body):
        ask = dict((p, 0) for p in OBJECTS + ('workers', 'tasks', 'irqs', 'caps'))
        for field in fields:
            if not field.strip():
                continue
            if LINE_RE.match(field):
                ask['irqs'] = max(ask['irqs'], 1)
                continue
            m = FIELD_RE.match(field)
            if not m or m.group(1) not in FIELDS:
                raise ValueError('TAP_ASK states `%s`, which is not `.<field> = <n>` over %s'
                                 % (field.strip(), ', '.join(FIELDS)))
            ask[m.group(1)] = max(ask[m.group(1)], value(m.group(2), consts, 'a TAP_ASK field'))
        for p in OBJECTS + ('workers', 'tasks', 'irqs'):
            need[p] = max(need[p], ask[p])
        probe = 0
        if ask['caps'] and not ask['notifies']:
            probe = 1
            need['notifies'] = max(need['notifies'], 1)
        need['caps'] = max(need['caps'],
                           sum(ask[p] for p in OBJECTS) + ask['irqs'] + ask['caps'] + probe)
        if ask['workers']:
            need['sems'] = max(need['sems'], 1)
            need['caps'] = max(need['caps'], 1)
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
        code_lines = code.split('\n')
        for fn, a, b in spans(code):
            body = '\n'.join(code_lines[a:b + 1])
            need = own_demand(body, consts)
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
    'void helper()\n{\n    TAP_ASK(.workers = TRIES);\n}\n'
    'void t_a()\n{\n    helper();\n}\n'
    'void t_b()\n{\n    TAP_ASK(.notifies = 1, .caps = 5);\n}\n'
    'void t_c()\n{\n    tap::skip("short of workers: %d of %d (%d)", 1, 9, -12);\n}\n'
    'void t_d()\n{\n    TAP_ASK(.sems = 2, .endpoints = 1);\n}\n'
    'constexpr unsigned CROWD = KICKOS_KERNEL_CORES + 1u;\n'
    'constexpr int CROWD_MORE = CROWD + 1;\n'
    'void t_f()\n{\n    TAP_ASK(.workers = CROWD_MORE);\n}\n'
    'void t_g()\n{\n    TAP_ASK(.workers = 2);\n}\n'
    'void t_h()\n{\n    TAP_ASK(.workers = 2);\n}\n'
    'void t_i()\n{\n    kos_cap_t line = KOS_CAP_NONE;\n'
    '    TAP_ASK_LINE(&line, .workers = 2, .tasks = 2, .sems = 1,\n'
    '                 .irq_line = KICKOS_IRQ_FREE_BASE + 3);\n}\n'
    'void t_j()\n{\n    TAP_ASK(.caps = 2);\n}\n'
    'void t_k()\n{\n    TAP_ASK(.notifies = 1, .caps = 2);\n}\n'
    'void t_l()\n{\n    kos_cap_t line = KOS_CAP_NONE;\n'
    '    TAP_ASK_LINE(&line, .irqs = 2, .irq_line = KICKOS_IRQ_FREE_BASE + 3);\n}\n'
    'void reg()\n{\n    TAP_ADD("a", t_a);\n    TAP_ADD("b", t_b);\n    TAP_ADD("i", t_i);\n'
    '    TAP_ADD("c", t_c);\n    TAP_ADD("d", t_d);\n    TAP_ADD("j", t_j);\n    TAP_ADD("k", t_k);\n'
    '    TAP_ADD("l", t_l);\n'
    '#if defined(X) && KICKOS_KERNEL_CORES > 1\n    TAP_ADD("f", t_f);\n#endif\n'
    '#if defined(KICKOS_ENABLE_SELFTEST)\n    TAP_ADD("g", t_g);\n#endif\n'
    '#if KICKOS_KERNEL_CORES > 1 \\\n    and not KICKOS_HAVE_ASPACE\n'
    '    TAP_ADD("h", t_h);\n#endif\n'
    '}\n']
PLANTED_CONFIG = {'KICKOS_KERNEL_CORES': 2}
ROOMY = {'workers': 5, 'tasks': 2, 'irqs': 2, 'caps': 6, 'sems': 2, 'mutexes': 1, 'endpoints': 1,
         'notifies': 1}


def ask(workers=0, tasks=0, irqs=0, caps=0, sems=0, endpoints=0, notifies=0):
    return {'workers': workers, 'tasks': tasks, 'irqs': irqs, 'caps': caps, 'sems': sems,
            'mutexes': 0, 'endpoints': endpoints, 'notifies': notifies}


def self_test():
    got = demands(PLANTED, PLANTED_CONFIG)
    want = {'a': ask(workers=5, caps=1, sems=1), 'b': ask(caps=6, notifies=1),
            'd': ask(caps=3, sems=2, endpoints=1), 'f': ask(workers=4, caps=1, sems=1),
            'i': ask(workers=2, tasks=2, irqs=1, caps=2, sems=1),
            'g': ask(workers=2, caps=1, sems=1), 'h': ask(workers=2, caps=1, sems=1),
            'j': ask(caps=3, notifies=1), 'k': ask(caps=3, notifies=1),
            'l': ask(irqs=2, caps=2)}
    if got != want:
        print('FAIL: planted arms read as %s, expected %s' % (got, want))
        return 1
    if short(got, ROOMY):
        print('FAIL: a posture supplying every demand expects %s to skip' % short(got, ROOMY))
        return 1
    # A semaphore budget one short of d's two, which a worker probe's gate alone still fits.
    for pool, arms in (('sems', ['d']), ('caps', ['b']), ('endpoints', ['d']), ('workers', ['a']),
                       ('tasks', ['i']), ('irqs', ['l'])):
        posture = dict(ROOMY)
        posture[pool] = ROOMY[pool] - 1
        if short(got, posture) != arms:
            print('FAIL: a posture one %s short expects %s to skip, not %s'
                  % (pool, short(got, posture), arms))
            return 1
    # The stated slots and the probe's own notification: three slots fit j, two do not.
    for caps, arms in ((3, []), (2, ['j', 'k'])):
        posture = dict(ROOMY)
        posture['caps'] = caps
        if [a for a in short(got, posture) if a in ('j', 'k')] != arms:
            print('FAIL: a posture of %d capability slots expects %s of j and k to skip, not %s'
                  % (caps, [a for a in short(got, posture) if a in ('j', 'k')], arms))
            return 1
    # Two lines held at once: one binding short of them refuses l, and not i's one line.
    posture = dict(ROOMY)
    posture['irqs'] = 0
    if short(got, posture) != ['i', 'l']:
        print('FAIL: a posture binding no line expects %s to skip, not i and l'
              % short(got, posture))
        return 1
    for field, what in (('.sems = n', 'a field naming a runtime value'),
                        ('.threads = 2', 'a field no ObjectDemand states'),
                        ('.sems = count(2)', 'a field calling a function')):
        bad = ['void t_e()\n{\n    TAP_ASK(%s);\n}\n'
               'void reg()\n{\n    TAP_ADD("e", t_e);\n}\n' % field]
        try:
            demands(bad)
        except ValueError:
            pass
        else:
            print('FAIL: a TAP_ASK of %s was read as a demand' % what)
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
    fail = count_controls()
    if fail:
        print('FAIL: %s' % fail)
        return 1
    print('PASS: a helper\'s ask, a named constant, a sum over a configured one, an object ask, '
          'one ask of workers, tasks, objects and a line, a count of lines held at once, and '
          'capability slots with the notification their probe holds are each read, a skip reason '
          'is not, an arm the configuration does not register is expected nowhere, a guard on a '
          'defined, undefined or valued macro is followed across a continued line, and a posture '
          'one short of any pool expects the arm asking it to skip; each region counts the '
          'registrations its guards compile, and a guard the configuration does not decide, an '
          '#else, a registration outside a region or in a form not counted, and one in another '
          'file are each refused')
    return 0


def region(n, body):
    return ('#if KICKOS_SELFTEST_REGION(%d)\n#define TAP_ADD(name, fn) tap::add(name, fn)\n#else\n'
            '#define TAP_ADD(name, fn) TAP_ELIDE(fn)\n#endif\n%s#undef TAP_ADD\n' % (n, body))


COUNTED_ONE = ('    TAP_ADD("a", t_a);\n'
               '    // TAP_ADD("commented", t_commented);\n'
               '#if KICKOS_KERNEL_CORES > 1 \\\n    and not KICKOS_HAVE_ASPACE\n'
               '    TAP_ADD_IRQ("b", t_b);\n#endif\n')
COUNTED_TWO = ('#if defined(X) && (KICKOS_HAVE_MPU || defined(SPARE))\n    TAP_ADD("c", t_c);\n#endif\n'
               '#if KICKOS_AMP_NODE\n#if KICKOS_SHARE != 0\n    TAP_ADD_PINNED("f", t_f);\n#endif\n'
               '#endif\n'
               '    TAP_ADD("g", t_g);\n')
COUNTED = {'main.cc': 'void selftest_main()\n{\n%s%s}\n' % (region(1, COUNTED_ONE),
                                                             region(2, COUNTED_TWO)),
           'selftest.h': '#define TAP_ADD_IRQ(name, fn) TAP_ADD(name, fn)\n'}
DROP = object()
COUNTED_CONFIG = {'KICKOS_KERNEL_CORES': 2, 'KICKOS_HAVE_ASPACE': 0, 'KICKOS_HAVE_MPU': 0,
                  'X': 1, 'SPARE': 0x101000, 'KICKOS_AMP_NODE': 0}


def counted_config(change):
    config = dict(COUNTED_CONFIG)
    for name, v in change.items():
        config[name] = v
        if v is DROP:
            del config[name]
    return config


def count_controls():
    """Why the planted regions are counted wrong, or None."""
    for change, want in (
            ({}, [2, 2]),
            ({'KICKOS_KERNEL_CORES': 1}, [1, 2]),
            ({'KICKOS_HAVE_MPU': 1, 'SPARE': UNDEFINED}, [2, 2]),
            ({'SPARE': UNDEFINED, 'KICKOS_HAVE_ASPACE': 1}, [1, 1]),
            ({'X': UNDEFINED, 'KICKOS_HAVE_MPU': DROP, 'SPARE': DROP}, [2, 1]),
            ({'KICKOS_AMP_NODE': 1, 'KICKOS_SHARE': 4096}, [2, 3]),
            ({'KICKOS_AMP_NODE': 1, 'KICKOS_SHARE': 0}, [2, 2])):
        try:
            got = arm_counts(COUNTED, counted_config(change))
        except ValueError as e:
            return 'the planted regions under %s were refused: %s' % (change, e)
        if got != want:
            return 'the planted regions under %s count %s, not %s' % (change, got, want)
    one = COUNTED['main.cc']
    reg = '    TAP_ADD("g", t_g);\n'
    for texts, change, what, why in (
            ({'main.cc': one}, {'KICKOS_HAVE_ASPACE': DROP}, 'a guard naming a macro no --config gives',
             'not decide'),
            ({'main.cc': one}, {'KICKOS_HAVE_MPU': 1, 'SPARE': DROP}, 'an || with one side undecided',
             'not decide'),
            ({'main.cc': one.replace('KICKOS_KERNEL_CORES > 1', 'KICKOS_FOO(1)')}, {},
             'a guard calling a function-like macro', 'not decide'),
            ({'main.cc': one.replace('KICKOS_KERNEL_CORES > 1', 'KICKOS_KERNEL_CORES + 1 > 2')}, {},
             'a guard doing arithmetic', 'not decide'),
            ({'main.cc': one.replace(reg, '#if X\n#else\n' + reg + '#endif\n')}, {},
             'a registration under an #else', '#elif or #else'),
            ({'main.cc': one.replace('#undef TAP_ADD\n#if KICKOS_SELFTEST_REGION(2)',
                                     '#undef TAP_ADD\n    TAP_ADD("h", t_h);\n'
                                     '#if KICKOS_SELFTEST_REGION(2)')}, {},
             'a registration outside every region', 'outside every region'),
            ({'main.cc': one.replace(reg, '    if (g_x) { TAP_ADD("g", t_g); }\n')}, {},
             'a registration a run-time condition decides', 'not one registration'),
            ({'main.cc': one.replace(reg, '    tap::add("g", t_g);\n')}, {},
             'an arm registered past TAP_ADD', 'not one registration'),
            ({'main.cc': one.replace(reg, '    TAP_ADD("g", t_g); TAP_ADD("i", t_i);\n')}, {},
             'two registrations on one line', 'not one registration'),
            ({'main.cc': one, 'other.cc': 'void reg()\n{\n    TAP_ADD("j", t_j);\n}\n'}, {},
             'an arm another file registers', 'only main.cc'),
            ({'main.cc': one, 'selftest.h': 'inline void reg()\n{\n    tap::add("j", t_j);\n}\n'}, {},
             'an arm a header registers', 'only main.cc')):
        try:
            got = arm_counts(texts, counted_config(change))
        except ValueError as e:
            if why not in str(e):
                return '%s was refused for another reason: %s' % (what, e)
            continue
        return '%s was counted as %s' % (what, got)
    return None


def main(argv):
    if argv[1:] == ['--self-test']:
        return self_test()
    src = argv[1]
    rest = argv[2:]
    config = {}
    while rest[:1] in (['--config'], ['--undefined']) and len(rest) > 1:
        name, eq, n = rest[1].partition('=')
        if rest[0] == '--undefined':
            if eq or not re.match(r'^[A-Za-z_]\w*$', name):
                print('FAIL: --undefined takes <name>, not %s' % rest[1], file=sys.stderr)
                return 1
            config[name] = UNDEFINED
        elif not re.match(r'^[A-Za-z_]\w*$', name) or (eq and not VALUE_RE.match(n)):
            print('FAIL: --config takes <name>[=<n>], not %s' % rest[1], file=sys.stderr)
            return 1
        elif eq:
            config[name] = int(n, 0)
        else:
            config[name] = 1
        rest = rest[2:]
    supply = None
    if rest and rest != ['--count']:
        if rest[0] != '--supply':
            print('FAIL: usage: selftest_demands.py <dir> [--config <name>[=<n>]...] '
                  '[--undefined <name>...] [--supply <pool>=<n>... | --count]', file=sys.stderr)
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
    texts = {}
    for name in sorted(os.listdir(src)):
        if name.endswith('.cc') or name.endswith('.h'):
            with open(os.path.join(src, name)) as fh:
                texts[name] = fh.read()
    if rest == ['--count']:
        try:
            counts = arm_counts(texts, config)
        except ValueError as e:
            print('FAIL: %s' % e, file=sys.stderr)
            return 1
        for n in counts:
            print(n)
        return 0
    try:
        found = demands([texts[name] for name in sorted(texts) if name.endswith('.cc')], config)
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
