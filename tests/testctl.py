#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# THE CONTROL CLIENT for long-running test tools (tests/testrun.py).
#
#   tests/testctl.py list                     every live run
#   tests/testctl.py status   [RUN]           progress, ETA, resume token,
#                                             what is running right now
#   tests/testctl.py failures [RUN]           the failures found so far
#   tests/testctl.py stop     [RUN]           graceful: finish in-flight,
#                                             checkpoint, print the token
#   tests/testctl.py pause | resume [RUN]
#   tests/testctl.py jobs N   [RUN]           change the parallelism
#
# RUN is a socket path or a substring of one (`int_enum`, a pid); omitted,
# it is the only live run, or an error naming the candidates.

import glob
import json
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testrun import runs_dir  # noqa: E402


def live_runs():
    out = []
    for p in sorted(glob.glob(os.path.join(runs_dir(), "*.sock"))):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(p)
            out.append(p)
        except OSError:
            try:
                os.unlink(p)        # a stale socket from a killed run
            except OSError:
                pass
        finally:
            s.close()
    return out


def pick(sel):
    runs = live_runs()
    if sel:
        runs = [r for r in runs if sel in r]
    if len(runs) != 1:
        raise SystemExit("testctl: %d matching run(s)%s" % (
            len(runs), "".join("\n  " + r for r in runs)))
    return runs[0]


def ask(path, req):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    f = s.makefile("rw")
    f.write(json.dumps(req) + "\n")
    f.flush()
    resp = json.loads(f.readline())
    s.close()
    return resp


def main():
    if len(sys.argv) < 2:
        print(__doc__ if __doc__ else "usage: testctl.py ACTION [RUN]")
        return 2
    act = sys.argv[1]
    if act == "list":
        for r in live_runs():
            st = ask(r, {"action": "status"})
            print("%s  %d/%d  ETA %ss  %d failure(s)" % (
                r, st["done"], st["total"], st["eta_s"], st["failures"]))
        return 0
    if act == "jobs":
        n = int(sys.argv[2])
        path = pick(sys.argv[3] if len(sys.argv) > 3 else None)
        print(json.dumps(ask(path, {"action": "jobs", "n": n}), indent=1))
        return 0
    path = pick(sys.argv[2] if len(sys.argv) > 2 else None)
    print(json.dumps(ask(path, {"action": act}), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
