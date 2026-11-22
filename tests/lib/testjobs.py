# SPDX-License-Identifier: BSD-2-Clause
"""
The Python face of tests/jobs - the ONE definition of the functional
test tools' worker count and priority. Nothing here computes a number
or picks a scheduling class itself: it asks jobs, so the formula
cannot drift between the shell and the Python tools.

    import testjobs
    testjobs.ensure_idle()        # first thing in main(): re-exec self
                                  # under the idle policy, once
    n = testjobs.count()          # the worker count
"""

import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_JOBS = os.path.join(os.path.dirname(_HERE), "jobs")


def count():
    """The worker count (tests/jobs count). Falls back to the CPU
    count only where jobs cannot run at all (no POSIX shell)."""
    try:
        out = subprocess.run(["sh", _JOBS, "count"], capture_output=True,
                             text=True, check=True).stdout.strip()
        return max(1, int(out))
    except (OSError, ValueError, subprocess.CalledProcessError):
        return max(1, (os.cpu_count() or 2) - 2)


def ensure_idle():
    """Re-exec the running script through `tests/jobs run`, which
    applies the idle scheduling/I-O class (or its nice fallback) and
    marks MYLANG_TEST_IDLED=1 so this happens once per process tree."""
    if os.environ.get("MYLANG_TEST_IDLED") == "1" or os.name != "posix":
        return
    script = os.path.abspath(sys.argv[0])
    try:
        os.execv("/bin/sh", ["/bin/sh", _JOBS, "run", sys.executable,
                             script] + sys.argv[1:])
    except OSError:
        return      # cannot re-exec: run at the inherited priority
