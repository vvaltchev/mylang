#!/bin/bash
# tw vs default (VM+JIT+splice) over every bench/ and samples/ program.
# The -rt suite is NOT this net: it caught neither of today's two JIT
# bugs, both of which a corpus program showed immediately.
cd /home/vlad/dev/mylang
BIN=${1:-build-claude/dbg/mylang}
bad=0; n=0
for f in bench/my/*.my samples/*; do
  [ -f "$f" ] || continue
  case "$f" in *rand_sort*|*shopping*|*phonebook*) continue ;; esac
  a=$(timeout 120 $BIN -tw "$f" 1 2>&1 | tail -3)
  b=$(timeout 120 $BIN     "$f" 1 2>&1 | tail -3)
  n=$((n+1))
  [ "$a" = "$b" ] || { echo "DIFF $f"; echo "  tw : $a"; echo "  jit: $b"; bad=$((bad+1)); }
done
echo "corpus differential: $((n-bad))/$n agree"
