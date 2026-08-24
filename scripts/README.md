# scripts/ — developer tools

Small, dependency-free helpers for working on the interpreter. They are
*tools*, not tests: the test nets live in `tests/` and CI runs those.
Nothing here is wired into `make` or CI.

| script | what it answers |
|---|---|
| `vdjcmp.sh OLD NEW [-v]` | "Did my JIT change alter the emitted machine code at all?" Compares `-vdj` across bench/my + samples + tests/functional. |
| `sabotage.sh FILE OLD NEW [CHECK]` | "Does my new test actually catch the bug it is for?" Applies a defect, rebuilds, runs the check, always restores. |

## `vdjcmp.sh` — the pure-restructuring oracle

```sh
make -j BUILD_DIR=build-claude/before OPT=1 ASSERTS=0   # at the old commit
make -j BUILD_DIR=build-claude/after  OPT=1 ASSERTS=0   # with your change
scripts/vdjcmp.sh build-claude/before/mylang build-claude/after/mylang
```

Byte-identical emitted code over the whole corpus proves a refactor
changed nothing, far more cheaply and far more strongly than arguing
from a green test suite. Use it whenever a change to `jit.cpp` is
*supposed* to be behaviour-preserving.

It normalises baked absolute addresses and rel32 call displacements,
which differ between any two separate links. The header comment records
the three ways getting that wrong produced a false "everything changed"
(and one way it produced a file differing from *itself* at random under
ASLR) — read it before adjusting the rules.

Build both sides the same way: `VM_HARDENING` and `ASSERTS` change what
is emitted.

## `sabotage.sh` — watched-failing, automated

```sh
scripts/sabotage.sh src/codegen.cpp \
    'lin[w] = known ? ((out[w] & ~def[w]) | use[w]) : all;' \
    'lin[w] = known ? ((out[w] & ~def[w]) | use[w]) : (all & 0);'
```

Exit 0 means the check failed, i.e. the test has teeth. **Exit 1 means
the check passed with the defect in place** — the test is blind to it,
which is the answer you most need and the one hand-running hides.

`OLD_TEXT` must be unique in the file; a sabotage applied at two sites
proves nothing about either. Override the build with `SABOTAGE_BUILD=`.
