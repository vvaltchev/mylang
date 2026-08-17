#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Feed the REPL hostile input and require a DEFINED outcome.

The no-crash sweep (#137) covered script files and `.myv` images. The REPL is
a THIRD front end and shares almost none of that path: its own incremental
inferencer with cross-input type commitment, its own auto-termination and
multi-line continuation, retained per-input ASTs, an OPEN WORLD where every
top-level name stays map-resident and redefinable, and a dozen `:`
meta-commands that parse arbitrary text. None of it had ever been fuzzed.

THE CONTRACT is the same one: an error is fine, an ERROR MESSAGE is fine,
`:quit` is fine - a SIGSEGV, a std::terminate or a hang is not. A REPL that
rejects a line must stay usable for the next one.

    tests/repl_fuzz.py ./build/mylang            # the default 1500 sessions
    tests/repl_fuzz.py ./build/mylang -n 6000
    tests/repl_fuzz.py ./build/mylang --save /tmp/bad

Run it against a DEBUG (ASan+UBSan) build - that is where a use-after-free in
the retained-AST / interned-pointer machinery shows up, and the REPL is where
this codebase has historically had those (see the cross-input template
instantiation bug in CLAUDE.md).

Stdlib only (the no-dependencies rule), like tests/nested_fuzz.py.
"""
import os
import random
import subprocess
import sys

# The generator is TEMPLATE-based rather than character-random on purpose:
# random bytes mostly bounce off the lexer, while these reach the passes that
# actually hold state across inputs - the type commitment, the retained ASTs,
# the template cache, the meta-commands.
FRAGMENTS = [
    # cross-input type commitment + :undef (the REPL-only state machine)
    'var {v} = {lit};', 'var {v} = "{s}";', '{v} = {lit};', '{v} = "{s}";',
    'var dyn {v} = {lit};', ':undef {v}', ':type {v}', ':type {v} + 1',
    # declarations that persist and can be REdefined
    'func {f}({v}) {{ return {v} + 1; }}', 'func {f}() => {lit};',
    'struct {S} {{ int a; }}', 'struct {S} {{ float a; str b; }}',
    'var {v} = {S}({lit});', 'var {v} = {f}({lit});',
    # the template cache across inputs
    'func {f}(a) {{ return a; }}', '{f}({lit});', '{f}("{s}");',
    # incomplete / unterminated - the continuation state machine
    'func {f}(', 'var {v} = [', 'var {v} = {{', '"unterminated',
    '/* unterminated', 'if ({lit}) {{', 'foreach (var q in',
    # meta-commands with junk arguments
    ':show {f}', ':show {v}', ':show {lit} + {lit}', ':tree var {v} = {lit};',
    ':analyze {v} = {lit};', ':globals', ':help {v}', ':help :{v}',
    ':trace {v} on', ':trace all on', ':trace all off', ':source /nonexistent',
    ':{v}', ':', '::', ':help', ':type', ':show', ':undef',
    # things that legitimately error - the REPL must survive each
    '{v}[{lit}];', '{v}.{v};', '{v}();', 'throw {lit};', '1/0;',
    'var {v} = {v};', 'return {lit};', 'break;', 'continue;', 'rethrow;',
    # depth and size, bounded well under the parser's MAX_NEST
    '(' * 60 + '1' + ')' * 60 + ';', '[' * 40 + ']' * 40 + ';',
    'var {v} = ' + '1+' * 200 + '1;',
    # bytes the line editor and lexer must not choke on
    '\t\t{v};', '   ', '#c', '\x01\x02{v};', 'var {v} = 0x;', 'var {v} = 2_;',
]
NAMES = ['a', 'b', 'x', 'q', 'v1', '_', 'print', 'len', 'int', 'i']
FUNCS = ['f', 'g', 'print', 'len', 'abs', 'mk']
STRUCTS = ['S', 'P', 'int', 'Q']
LITS = ['0', '1', '-1', '9223372036854775807', '-9223372036854775808',
        '3.5', 'true', 'none', '[]', '{}', '[1,2]', '{"k":1}']
STRS = ['', 'x', 'a b', '\\n', '%s%d', 'q' * 40]


def session(rnd, nlines):
    out = []
    for _ in range(nlines):
        t = rnd.choice(FRAGMENTS)
        out.append(t.format(v=rnd.choice(NAMES), f=rnd.choice(FUNCS),
                            S=rnd.choice(STRUCTS), lit=rnd.choice(LITS),
                            s=rnd.choice(STRS)))
    out.append(':quit')
    return '\n'.join(out) + '\n'


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    n = 1500
    savedir = 'repl-fuzz-bad'
    if '-n' in sys.argv:
        n = int(sys.argv[sys.argv.index('-n') + 1])
    if '--save' in sys.argv:
        savedir = sys.argv[sys.argv.index('--save') + 1]

    rnd = random.Random(20260809)
    hangs = crashes = 0
    for i in range(n):
        src = session(rnd, rnd.randrange(1, 9))
        try:
            r = subprocess.run([binary, '--repl'], input=src.encode(),
                               timeout=15, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)
            rc, err = r.returncode, r.stderr.decode('utf-8', 'replace')
        except subprocess.TimeoutExpired:
            rc, err = None, ''
        # The REPL reports errors INLINE and keeps going, so a session that
        # ends any way other than by a signal is a defined outcome.
        if rc is None or rc < 0 or rc not in (0, 1):
            bad = 'HANG' if rc is None else 'CRASH rc=%s' % rc
            os.makedirs(savedir, exist_ok=True)
            path = os.path.join(savedir, 'session-%d.txt' % i)
            open(path, 'w').write(src)
            if rc is None:
                hangs += 1
            else:
                crashes += 1
            print('  %-12s #%d  %s  -> %s'
                  % (bad, i, err.strip().splitlines()[-1][:150]
                     if err.strip() else '', path))

    print('\n=== %d hangs, %d crashes over %d sessions ==='
          % (hangs, crashes, n))
    return 1 if (hangs or crashes) else 0


if __name__ == '__main__':
    sys.exit(main())
