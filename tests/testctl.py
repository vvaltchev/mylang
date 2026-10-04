#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# THE CONTROL CLIENT for long-running test tools (tests/testrun.py).
#
#   tests/testctl.py [list]                   EVERY running test tool, one
#                                             line each: its PERCENTAGE,
#                                             done/total, phase, elapsed,
#                                             ETA, failures (the default)
#   tests/testctl.py watch [SECS]             the same, refreshed (Ctrl-C)
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
#
# Every long test tool serves a status socket: a resumable `Run` or a
# `Monitor` (tests/testrun.py). A Monitor answers `status` only.

import glob
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testrun import runs_dir, fmt_secs, percent_of  # noqa: E402


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


def table():
    rows = []
    for r in live_runs():
        try:
            st = ask(r, {"action": "status"})
        except (OSError, ValueError):
            continue                # it finished between list and ask
        rows.append(st)
    if not rows:
        return "no test tool is running"
    out = ["%-12s %7s %6s %13s  %-14s %9s %9s %5s  %s"
           % ("TOOL", "PID", "%", "DONE/TOTAL", "PHASE", "ELAPSED",
              "ETA", "FAIL", "NOW")]
    for st in rows:
        cur = st.get("current") or ", ".join(st.get("running", [])[:2])
        out.append("%-12s %7d %5.1f%% %13s  %-14s %9s %9s %5d  %s" % (
            st["name"][:12], st["pid"],
            st.get("percent", percent_of(st.get("done", 0),
                                         st.get("total", 0))),
            "%d/%d" % (st.get("done", 0), st.get("total", 0)),
            (st.get("phase") or "-")[:14],
            fmt_secs(st.get("elapsed_s")),
            st.get("eta") or fmt_secs(st.get("eta_s")),
            st.get("failures", 0), (cur or "")[:60]))
    return "\n".join(out)


def main():
    act = sys.argv[1] if len(sys.argv) > 1 else "list"
    if act in ("-h", "--help", "help"):
        print(__doc__ if __doc__ else "usage: testctl.py ACTION [RUN]")
        return 0
    if act == "list":
        print(table())
        return 0
    if act == "watch":
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
        try:
            while True:
                print("\033[H\033[2J" + time.strftime("%H:%M:%S  ")
                      + "(Ctrl-C to quit)\n" + table(), flush=True)
                time.sleep(secs)
        except KeyboardInterrupt:
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
