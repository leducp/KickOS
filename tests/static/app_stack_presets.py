# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Witnesses the presets= lists of tests/static/app_stack_roots.txt without building: on each
# trap_redzone preset of an arch that declares a thread, does that preset's tree link the
# thread's image? A listed preset that does not, and a gated preset that does and is not listed,
# both fail.
#
# The answer comes from the build's own facts. The preset's KICKOS_* cache variables, resolved
# through `inherits` by tests/static/preset_boards.cmake, go to tools/kconfig/genconfig.py as
# the root CMakeLists offers them, and the conditions are read out of the CMake files that
# select the image: every depth-0 if() in the image's own file that returns, the call that
# enters its directory (add_subdirectory, or a macro whose body is one if() around that), and
# every if() enclosing that call, up to user/apps. A condition is evaluated against the
# generated kickos_config.cmake, or a variable the root CMakeLists sets once at depth 0.
#
# NOT MODELLED: what decides whether user/apps is entered at all (KICKOS_BUILD_APPS, the chip
# target), and a condition inside the image's file other than a returning guard. A selection
# or a guard of any other shape is refused rather than read as unconditional.

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import app_stack  # noqa: E402
import trap_redzone as T  # noqa: E402

APPS = 'user/apps'
FALSE_CONSTANTS = ('', '0', 'OFF', 'NO', 'FALSE', 'N', 'IGNORE', 'NOTFOUND')
OPENERS = {'if': 'endif', 'foreach': 'endforeach', 'while': 'endwhile',
           'function': 'endfunction', 'macro': 'endmacro', 'block': 'endblock'}


def die(msg):
    raise T.Bad(msg)


# --- CMake reading -------------------------------------------------------------------------

def commands(path):
    """[(line, name, [(token, quoted)])] of a CMake file, comments gone."""
    text = open(path).read()
    out = []
    i = 0
    line = 1
    n = len(text)
    while i < n:
        c = text[i]
        if c == '\n':
            line += 1
            i += 1
        elif c == '#':
            m = re.match(r'#\[(=*)\[', text[i:])
            if m:
                end = text.find(']' + m.group(1) + ']', i)
                if end < 0:
                    die('%s:%d: an unterminated bracket comment' % (path, line))
                line += text.count('\n', i, end)
                i = end + len(m.group(1)) + 2
            else:
                while i < n and text[i] != '\n':
                    i += 1
        elif c.isalpha() or c == '_':
            m = re.match(r'[A-Za-z_][A-Za-z0-9_]*', text[i:])
            name = m.group(0)
            j = i + len(name)
            while j < n and text[j] in ' \t':
                j += 1
            if j >= n or text[j] != '(':
                die('%s:%d: "%s" is no command call this reader knows' % (path, line, name))
            start = line
            tokens, i, line = arguments(path, text, j + 1, line)
            out.append((start, name.lower(), tokens))
        else:
            i += 1
    return out


def arguments(path, text, i, line):
    tokens = []
    depth = 0
    n = len(text)
    word = ''
    while i < n:
        c = text[i]
        if c == '"' and word:
            j = text.find('"', i + 1)
            if j < 0:
                die('%s:%d: an unterminated quote inside an unquoted argument' % (path, line))
            line += text.count('\n', i, j)
            word += text[i:j + 1]
            i = j + 1
            continue
        if c == '"':
            j = i + 1
            val = ''
            while j < n and text[j] != '"':
                if text[j] == '\\' and j + 1 < n:
                    val += text[j:j + 2]
                    j += 2
                    continue
                if text[j] == '\n':
                    line += 1
                val += text[j]
                j += 1
            if j >= n:
                die('%s:%d: an unterminated quoted argument' % (path, line))
            tokens.append((val, True))
            i = j + 1
            continue
        if c in ' \t\n()#':
            if word:
                tokens.append((word, False))
                word = ''
            if c == '\n':
                line += 1
            elif c == '#':
                while i < n and text[i] != '\n':
                    i += 1
                continue
            elif c == '(':
                depth += 1
                tokens.append(('(', False))
            elif c == ')':
                if depth == 0:
                    return tokens, i + 1, line
                depth -= 1
                tokens.append((')', False))
            i += 1
            continue
        m = re.match(r'\[(=*)\[', text[i:])
        if m and not word:
            end = text.find(']' + m.group(1) + ']', i)
            if end < 0:
                die('%s:%d: an unterminated bracket argument' % (path, line))
            tokens.append((text[i + len(m.group(0)):end], True))
            line += text.count('\n', i, end)
            i = end + len(m.group(1)) + 2
            continue
        word += c
        i += 1
    die('%s:%d: an unterminated command' % (path, line))


def blocks(path, cmds):
    """Pairs each command with the stack of openers enclosing it: [(cmd, [(cmd, branch)])]."""
    stack = []
    out = []
    for cmd in cmds:
        _line, name, _tok = cmd
        if name in OPENERS.values():
            if not stack or OPENERS[stack[-1][0][1]] != name:
                die('%s:%d: %s() closes nothing' % (path, cmd[0], name))
            stack.pop()
            continue
        if name in ('elseif', 'else'):
            if not stack or stack[-1][0][1] != 'if':
                die('%s:%d: %s() outside an if()' % (path, cmd[0], name))
            stack[-1] = (stack[-1][0], name)
            out.append((cmd, list(stack[:-1])))
            continue
        out.append((cmd, list(stack)))
        if name in OPENERS:
            stack.append((cmd, 'if'))
    if stack:
        die('%s:%d: %s() is never closed' % (path, stack[-1][0][0], stack[-1][0][1]))
    return out


# --- conditions ----------------------------------------------------------------------------

class Cond(object):
    """An if() condition, kept with where it came from and whether it must hold or must not."""

    def __init__(self, where, tokens, holds):
        self.where = where
        self.tokens = tokens
        self.holds = holds

    def text(self):
        words = []
        for t, q in self.tokens:
            if q:
                words.append('"%s"' % t)
            else:
                words.append(t)
        body = ' '.join(words)
        if self.holds:
            return body
        return 'NOT (%s)' % body

    def value(self, env):
        got = Expr(self, env).run()
        if self.holds:
            return got
        return not got


class Expr(object):
    """if() over NOT, AND, OR, parentheses, a bare variable and STREQUAL; nothing else."""

    def __init__(self, cond, env):
        self.cond = cond
        self.env = env
        self.toks = cond.tokens
        self.i = 0

    def refuse(self, why):
        die('%s: cannot evaluate if(%s): %s' % (self.cond.where, self.cond.text(), why))

    def peek(self):
        if self.i < len(self.toks) and not self.toks[self.i][1]:
            return self.toks[self.i][0]
        return None

    def run(self):
        got = self.disjunction()
        if self.i != len(self.toks):
            self.refuse('a token past the end of the expression')
        return got

    def disjunction(self):
        got = self.conjunction()
        while self.peek() == 'OR':
            self.i += 1
            rhs = self.conjunction()
            got = got or rhs
        return got

    def conjunction(self):
        got = self.negation()
        while self.peek() == 'AND':
            self.i += 1
            rhs = self.negation()
            got = got and rhs
        return got

    def negation(self):
        if self.peek() == 'NOT':
            self.i += 1
            return not self.negation()
        return self.atom()

    def var(self, name):
        if name not in self.env:
            self.refuse('%s is neither in the generated configuration nor set once by the'
                        ' root' % name)
        return self.env[name]

    def atom(self):
        if self.i >= len(self.toks):
            self.refuse('the expression ends early')
        tok, quoted = self.toks[self.i]
        self.i += 1
        if tok == '(' and not quoted:
            got = self.disjunction()
            if self.peek() != ')':
                self.refuse('an unclosed parenthesis')
            self.i += 1
            return got
        if quoted or not re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', tok):
            self.refuse('"%s" is no variable' % tok)
        if self.peek() == 'STREQUAL':
            self.i += 1
            if self.i >= len(self.toks):
                self.refuse('STREQUAL with no right-hand side')
            rhs, rquoted = self.toks[self.i]
            self.i += 1
            if not rquoted:
                self.refuse('STREQUAL against the unquoted %s' % rhs)
            return self.var(tok) == rhs
        if self.peek() is not None and self.peek() not in ('AND', 'OR', ')'):
            self.refuse('the operator %s' % self.peek())
        value = self.var(tok)
        return not (value.upper() in FALSE_CONSTANTS or value.upper().endswith('-NOTFOUND'))


def named(tokens, word):
    pat = re.compile(r'(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])' % re.escape(word))
    return any(pat.search(t) for t, _q in tokens)


def image_file(src, image):
    """The one CMakeLists.txt under user/apps that both adds an executable and names image."""
    found = []
    for root, dirs, files in os.walk(os.path.join(src, APPS)):
        dirs.sort()
        if 'CMakeLists.txt' not in files:
            continue
        path = os.path.join(root, 'CMakeLists.txt')
        cmds = commands(path)
        if not any(name == 'add_executable' for _l, name, _t in cmds):
            continue
        if any(named(tok, image) for _l, _n, tok in cmds):
            found.append(path)
    if len(found) != 1:
        die('%s: the image %s is named by %d app files under %s that add an executable (%s),'
            ' so its own conditions cannot be read' % (src, image, len(found), APPS,
                                                         ', '.join(found) or 'none'))
    return found[0]


def guards(path):
    """The image file's own conditions: each depth-0 if() whose branch returns must not hold."""
    out = []
    for (line, name, tok), enclosing in blocks(path, commands(path)):
        if name != 'return':
            continue
        if len(enclosing) != 1 or enclosing[0][0][1] != 'if' or enclosing[0][1] != 'if':
            die('%s:%d: a return() other than in the first branch of a depth-0 if(), which'
                ' this gate cannot read as a guard' % (path, line))
        opener = enclosing[0][0]
        out.append(Cond('%s:%d' % (path, opener[0]), opener[2], False))
    return out


def macro_condition(path, macro):
    """A selection macro's one condition, when its body is if() around a foreach() of
    add_subdirectory and nothing else."""
    body = None
    shape = []
    cond = None
    for (line, name, tok), enclosing in blocks(path, commands(path)):
        if name == 'macro' and tok and tok[0][0].lower() == macro:
            body = line
            continue
        if body is None or not enclosing or enclosing[0][0][0] != body:
            continue
        shape.append(name)
        if name == 'if' and len(enclosing) == 1:
            cond = Cond('%s:%d' % (path, line), tok, True)
    if body is None:
        return None
    if shape != ['if', 'foreach', 'add_subdirectory']:
        die('%s:%d: the macro %s is no if() around a foreach() of add_subdirectory, so the'
            ' condition it enters a directory under cannot be read' % (path, body, macro))
    return cond


def selection(src, appdir):
    """The conditions under which each directory from appdir up to user/apps is entered."""
    out = []
    top = os.path.join(src, APPS)
    here = appdir
    while os.path.normpath(here) != os.path.normpath(top):
        parent = os.path.dirname(here)
        if not parent.startswith(top):
            die('%s is not under %s' % (appdir, top))
        path = os.path.join(parent, 'CMakeLists.txt')
        leaf = os.path.basename(here)
        hits = []
        for (line, name, tok), enclosing in blocks(path, commands(path)):
            if name == 'macro' or not any(t == leaf for t, _q in tok):
                continue
            hits.append((line, name, tok, enclosing))
        if len(hits) != 1:
            die('%s: %d calls name the directory %s, so how it is entered cannot be read'
                % (path, len(hits), leaf))
        line, name, tok, enclosing = hits[0]
        if name != 'add_subdirectory':
            gate = macro_condition(path, name)
            if gate is None:
                gate = macro_condition(os.path.join(top, 'CMakeLists.txt'), name)
            if gate is None:
                die('%s:%d: %s() enters %s and is no macro of %s or %s'
                    % (path, line, name, leaf, path, os.path.join(top, 'CMakeLists.txt')))
            out.append(gate)
        for opener, branch in enclosing:
            if opener[1] != 'if' or branch != 'if':
                die('%s:%d: %s is entered inside the %s() branch of the %s() at line %d,'
                    ' which this gate cannot evaluate'
                    % (path, line, leaf, branch, opener[1], opener[0]))
            out.append(Cond('%s:%d' % (path, opener[0]), opener[2], True))
        here = parent
    return out


def root_constants(src):
    """Variables the root CMakeLists sets exactly once, at depth 0, to a plain value."""
    path = os.path.join(src, 'CMakeLists.txt')
    count = {}
    plain = {}
    for (_line, name, tok), enclosing in blocks(path, commands(path)):
        if name != 'set' or not tok:
            continue
        var = tok[0][0]
        count[var] = count.get(var, 0) + 1
        if not enclosing and len(tok) == 2:
            plain[var] = tok[1][0]
    return dict((k, v) for k, v in plain.items() if count[k] == 1)


# --- the preset's configuration ------------------------------------------------------------

def read_fragment(path):
    """The set(NAME VALUE) lines of a generated kickos_config.cmake."""
    env = {}
    for line in open(path):
        m = re.match(r'^set\(([A-Za-z_][A-Za-z0-9_]*) (.*)\)$', line.strip())
        if not m:
            continue
        value = m.group(2)
        if value.startswith('"') and value.endswith('"'):
            value = re.sub(r'\\(.)', r'\1', value[1:-1])
        env[m.group(1)] = value
    if 'KICKOS_BOARD' not in env:
        die('%s sets no KICKOS_BOARD, so it is no configuration the generator wrote' % path)
    return env


def requests(cache):
    """The genconfig requests the root CMakeLists builds from these cache variables."""
    # Mirrors the root CMakeLists' list, but offers every name: an assert only adds a refusal.
    out = []
    for var, (kind, value) in sorted(cache.items()):
        if kind == 'INTERNAL':
            continue
        out.append('offer:%s=%s' % (var, value))
    if 'KICKOS_CONSOLE' in cache:
        out.append('CONFIG_CONSOLE_%s=y' % cache['KICKOS_CONSOLE'][1].upper())
    if 'KICKOS_TELEMETRY' in cache:
        out.append('CONFIG_TELEMETRY_%s=y' % cache['KICKOS_TELEMETRY'][1].upper())
    return out


def configure(src, python, scratch, preset, cache):
    variant = cache.get('KICKOS_CONFIG_VARIANT', ('', 'base'))[1]
    board = cache.get('KICKOS_BOARD', ('', ''))[1]
    if not board:
        die('the preset %s resolves no KICKOS_BOARD' % preset)
    defconfig = os.path.join(src, 'boards', board, 'configs', variant, 'defconfig')
    gendir = os.path.join(scratch, preset)
    if os.path.exists(gendir):
        die('%s exists, and genconfig would resolve its live .config instead' % gendir)
    r = subprocess.run([python, os.path.join(src, 'tools', 'kconfig', 'genconfig.py'), src,
                        defconfig, gendir] + requests(cache),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        die('genconfig refused the preset %s:\n%s'
            % (preset, r.stdout.decode('utf-8', 'replace')))
    return read_fragment(os.path.join(gendir, 'kickos_config.cmake'))


# --- the verdict ---------------------------------------------------------------------------

def links(conds, env):
    """None when every condition holds, else the first that does not."""
    for c in conds:
        if not c.value(env):
            return c
    return None


def judge(threads, gated, conds, envs):
    """threads: {arch: [(image, name, presets)]}; gated: {arch: [preset]};
    conds: {image: [Cond]}; envs: {preset: env}. Returns (findings, confirmed, absent)."""
    findings = []
    confirmed = []
    absent = []
    for arch in sorted(threads):
        if not gated.get(arch):
            findings.append('%s declares threads and trap_redzone gates no preset of it' % arch)
            continue
        for image, name, listed in threads[arch]:
            for preset in sorted(set(gated[arch]) | set(listed)):
                if preset not in envs:
                    findings.append('%s/%s: no configuration was read for %s'
                                    % (image, name, preset))
                    continue
                miss = links(conds[image], envs[preset])
                if preset in listed and miss is not None:
                    findings.append('LISTED, NOT LINKED: %s/%s names %s, whose tree does not'
                                    ' link %s: %s, if(%s)'
                                    % (image, name, preset, image, miss.where, miss.text()))
                elif preset not in listed and miss is None:
                    findings.append('LINKED, NOT LISTED: %s/%s leaves out %s, a trap_redzone'
                                    ' preset of %s whose tree links %s'
                                    % (image, name, preset, arch, image))
                elif miss is None:
                    confirmed.append((arch, image, name, preset))
                else:
                    absent.append((arch, image, name, preset, miss))
    if not confirmed and not findings:
        findings.append('nothing was judged: no thread, or no preset, was read')
    return findings, confirmed, absent


def read_cache(path):
    out = {}
    for line in open(path):
        f = line.rstrip('\n').split('\t')
        if len(f) != 4:
            die('%s: "%s" is no <preset> <variable> <type> <value> line' % (path, line.strip()))
        out.setdefault(f[0], {})[f[1]] = (f[2], f[3])
    if not out:
        die('%s resolves no cache variable for any preset' % path)
    return out


def main(argv):
    if len(argv) != 6:
        sys.stderr.write('usage: app_stack_presets.py <src> <kconfig-python> <decl> <roots>'
                         ' <preset-cache.tsv> <scratch>\n')
        return 2
    src, python, decl, roots, cache_tsv, scratch = argv
    src = os.path.abspath(src)
    caches = read_cache(cache_tsv)
    gated = {}
    for _n, f, _r in T.records(roots):
        if len(f) == 3 and f[0] == 'preset':
            gated.setdefault(f[1], []).append(f[2])
    threads = {}
    for arch in sorted(set(f[1] for _n, f, _r in T.records(decl) if len(f) > 1)):
        for image, name, _root, _stack, presets in app_stack.Decl(decl, arch).threads:
            threads.setdefault(arch, []).append((image, name, presets))
    if not threads:
        die('%s declares no thread' % decl)
    constants = root_constants(src)
    conds = {}
    for arch in threads:
        for image, _name, _presets in threads[arch]:
            if image not in conds:
                path = image_file(src, image)
                conds[image] = guards(path) + selection(src, os.path.dirname(path))
                for c in conds[image]:
                    c.where = os.path.relpath(c.where, src)
                    print('app_stack_presets: %s needs if(%s) at %s'
                          % (image, c.text(), c.where))
    envs = {}

    def env_of(preset):
        if preset not in envs:
            if preset not in caches:
                die('%s is no visible configure preset, and check_trap_redzone.sh configures'
                    ' --preset by that name' % preset)
            env = dict(constants)
            fragment = configure(src, python, scratch, preset, caches[preset])
            for k in fragment:
                if k in env:
                    die('%s is both generated by Kconfig and set by the root' % k)
            env.update(fragment)
            envs[preset] = env
        return envs[preset]

    for arch in threads:
        for preset in sorted(set(gated.get(arch, [])).union(*[p for _i, _n, p in threads[arch]])):
            env_of(preset)
    for arch in sorted(set(gated) - set(threads)):
        for image in sorted(conds):
            on = [p for p in sorted(gated[arch]) if links(conds[image], env_of(p)) is None]
            if on:
                print('app_stack_presets: UNBOUNDED: %s declares no thread, and %d of its'
                      ' trap_redzone preset(s) link %s: %s'
                      % (arch, len(on), image, ' '.join(on)))
    findings, confirmed, absent = judge(threads, gated, conds, envs)
    for arch, image, name, preset in confirmed:
        print('app_stack_presets: %s %s/%s linked on %s' % (arch, image, name, preset))
    for arch, image, name, preset, miss in absent:
        print('app_stack_presets: %s %s/%s not linked on %s: %s, if(%s)'
              % (arch, image, name, preset, miss.where, miss.text()))
    for f in findings:
        print('FAIL: %s' % f)
    if findings:
        return 1
    print('app_stack_presets: OK, %d listed preset(s) link their thread' % len(confirmed))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except T.Bad as e:
        sys.stderr.write('FAIL: %s\n' % e)
        sys.exit(2)
