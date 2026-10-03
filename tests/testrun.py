# SPDX-License-Identifier: BSD-2-Clause
#
# THE LONG-RUNNING TEST HARNESS (#107): progress, control, resume.
#
# Any test tool whose run can take minutes or hours drives its work through
# a `Run`, which gives it, for free:
#
#   PROGRESS   a heartbeat line on stderr every `heartbeat` seconds (done /
#              total, rate, ETA, failures) - the CI log's view;
#   CONTROL    a unix-domain socket the run listens on, registered in the
#              runs directory (tests/testctl.py list | status | failures |
#              stop | pause | resume | jobs N). One JSON object per line in
#              each direction, so a new action is one more `elif`;
#   RESUME     the work is an ORDERED, DETERMINISTIC sequence of items, so
#              progress is one integer: the LOW-WATER MARK, the lowest index
#              not yet completed. Workers take items in order with a bounded
#              window of outstanding ones; everything below the mark is done,
#              and on resume anything above it is simply done again - the
#              waste is bounded by the window (seconds), and no worker ever
#              waits at a barrier for a straggler. The token is
#              `<mark>@<fingerprint>`, the fingerprint hashing whatever makes
#              item i what it is (the binary, the inputs, the options, the
#              discovered plan): resuming against a different plan is
#              REFUSED, never silently skipped.
#
# A graceful stop (the `stop` action, SIGINT or SIGTERM) dispatches nothing
# new, lets the in-flight items finish, writes the checkpoint (the mark and
# every failure found so far, so a resumed run still reports them) and
# prints the resume token.
#
# Stdlib only. Single-host. The runs directory is $MYLANG_TEST_RUNS, else
# $XDG_RUNTIME_DIR/mylang-tests, else /tmp/mylang-tests-<uid>.

import concurrent.futures as cf
import hashlib
import json
import os
import signal
import socket
import sys
import threading
import time


def runs_dir():
    d = os.environ.get("MYLANG_TEST_RUNS")
    if not d:
        base = os.environ.get("XDG_RUNTIME_DIR")
        d = (os.path.join(base, "mylang-tests") if base
             else "/tmp/mylang-tests-%d" % os.getuid())
    os.makedirs(d, exist_ok=True)
    return d


def fingerprint(*parts):
    h = hashlib.sha1()
    for p in parts:
        if isinstance(p, (bytes, bytearray)):
            h.update(p)
        else:
            h.update(str(p).encode())
        h.update(b"\0")
    return h.hexdigest()[:16]


def file_digest(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def parse_token(token):
    """`<mark>@<fingerprint>` -> (mark, fingerprint)."""
    mark, _, fp = token.partition("@")
    return int(mark), fp


class Run:
    """Drives `work(i, item) -> failure-text-or-None` over `items`.

    `name` labels the run (tests/testctl.py list), `fp` is the plan
    fingerprint, `resume` an optional token. `describe(item)` renders an
    item for status displays."""

    def __init__(self, name, items, work, fp, jobs, resume=None,
                 heartbeat=60.0, describe=str, window_per_job=4):
        self.name = name
        self.items = items
        self.work = work
        self.fp = fp
        self.jobs = max(1, jobs)
        self.heartbeat = heartbeat
        self.describe = describe
        self.window_per_job = window_per_job
        self.lock = threading.Lock()
        self.start = 0
        self.failures = []
        self.sock_path = os.path.join(
            runs_dir(), "%s-%d.sock" % (name, os.getpid()))
        self.ckpt_path = os.path.join(
            runs_dir(), "%s-%s.checkpoint.json" % (name, fp))
        if resume:
            mark, rfp = parse_token(resume)
            if rfp != fp:
                raise SystemExit(
                    "%s: resume token is for a different plan (%s, this run "
                    "is %s) - the binary, the inputs or the options changed"
                    % (name, rfp, fp))
            self.start = mark
            self.failures = self._load_failures(mark)
        self.mark = self.start          # low-water mark
        self.done = set()               # completed indices >= mark
        self.running = {}               # index -> start time
        self.ndone = 0
        self.t0 = time.time()
        self.stopping = False
        self.paused = threading.Event()
        self.paused.set()

    # -- checkpoint ----------------------------------------------------
    def _load_failures(self, mark):
        try:
            with open(self.ckpt_path) as f:
                c = json.load(f)
            if c.get("fp") == self.fp and c.get("mark", -1) >= mark:
                return list(c.get("failures", []))
        except (OSError, ValueError):
            pass
        return []

    def token(self):
        return "%d@%s" % (self.mark, self.fp)

    def _write_checkpoint(self):
        with self.lock:
            data = {"fp": self.fp, "mark": self.mark,
                    "total": len(self.items),
                    "failures": self.failures}
        tmp = self.ckpt_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f)
        os.replace(tmp, self.ckpt_path)

    # -- status ----------------------------------------------------------
    def status(self):
        with self.lock:
            el = time.time() - self.t0
            rate = self.ndone / el if el > 0 else 0.0
            left = len(self.items) - self.mark - len(self.done)
            return {
                "name": self.name, "pid": os.getpid(),
                "total": len(self.items), "mark": self.mark,
                "done": self.mark + len(self.done),
                "this_run_done": self.ndone,
                "elapsed_s": round(el, 1),
                "rate_per_s": round(rate, 2),
                "eta_s": round(left / rate, 1) if rate > 0 else None,
                "failures": len(self.failures),
                "jobs": self.jobs,
                "paused": not self.paused.is_set(),
                "stopping": self.stopping,
                "token": "%d@%s" % (self.mark, self.fp),
                "running": [self.describe(self.items[i])
                            for i in sorted(self.running)][:16],
            }

    def _line(self):
        s = self.status()
        eta = ("%dm%02ds" % divmod(int(s["eta_s"]), 60)
               if s["eta_s"] is not None else "?")
        return ("[%s] %d/%d done, %.1f/s, ETA %s, %d failure(s), "
                "resume %s" % (self.name, s["done"], s["total"],
                               s["rate_per_s"], eta, s["failures"],
                               s["token"]))

    # -- control socket ----------------------------------------------------
    def _serve(self, srv):
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=self._client, args=(conn,),
                             daemon=True).start()

    def _client(self, conn):
        with conn:
            f = conn.makefile("rw")
            for line in f:
                try:
                    req = json.loads(line)
                    act = req.get("action")
                    if act == "status":
                        resp = self.status()
                    elif act == "failures":
                        with self.lock:
                            resp = {"failures": list(self.failures)}
                    elif act == "stop":
                        self.request_stop()
                        resp = {"ok": True, "stopping": True}
                    elif act == "pause":
                        self.paused.clear()
                        resp = {"ok": True, "paused": True}
                    elif act == "resume":
                        self.paused.set()
                        resp = {"ok": True, "paused": False}
                    elif act == "jobs":
                        with self.lock:
                            self.jobs = max(1, int(req.get("n", self.jobs)))
                        resp = {"ok": True, "jobs": self.jobs,
                                "note": "applies to newly dispatched items"}
                    else:
                        resp = {"error": "unknown action %r" % act}
                except (ValueError, TypeError) as e:
                    resp = {"error": str(e)}
                f.write(json.dumps(resp) + "\n")
                f.flush()

    def request_stop(self):
        with self.lock:
            self.stopping = True
        self.paused.set()

    # -- the run -----------------------------------------------------------
    def _complete(self, i, failure):
        with self.lock:
            self.running.pop(i, None)
            self.ndone += 1
            if failure:
                self.failures.append(failure)
            self.done.add(i)
            while self.mark in self.done:
                self.done.discard(self.mark)
                self.mark += 1

    def execute(self):
        """Run every item from the resume point; returns the failures.
        On a graceful stop, prints the resume token and returns early."""
        try:
            os.unlink(self.sock_path)
        except OSError:
            pass
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(self.sock_path)
        srv.listen(8)
        threading.Thread(target=self._serve, args=(srv,), daemon=True).start()
        old = {s: signal.signal(s, lambda *_: self.request_stop())
               for s in (signal.SIGINT, signal.SIGTERM)}
        print("[%s] %d item(s) from %d; control socket %s"
              % (self.name, len(self.items), self.start, self.sock_path),
              file=sys.stderr, flush=True)
        stop_beat = threading.Event()

        def beat():
            while not stop_beat.wait(self.heartbeat):
                print(self._line(), file=sys.stderr, flush=True)
                self._write_checkpoint()

        threading.Thread(target=beat, daemon=True).start()
        try:
            with cf.ThreadPoolExecutor(max_workers=64) as ex:
                pending = set()
                nxt = self.start
                while nxt < len(self.items) or pending:
                    self.paused.wait()
                    with self.lock:
                        stopping = self.stopping
                        window = self.jobs * self.window_per_job
                        jobs = self.jobs
                    while (not stopping and nxt < len(self.items)
                           and len(pending) < jobs
                           and nxt - self.mark < window):
                        i = nxt
                        nxt += 1
                        with self.lock:
                            self.running[i] = time.time()
                        fut = ex.submit(self.work, i, self.items[i])
                        fut.idx = i
                        pending.add(fut)
                    if not pending:
                        break
                    finished, pending = cf.wait(
                        pending, timeout=1.0,
                        return_when=cf.FIRST_COMPLETED)
                    for fut in finished:
                        self._complete(fut.idx, fut.result())
        finally:
            stop_beat.set()
            for s, h in old.items():
                signal.signal(s, h)
            srv.close()
            try:
                os.unlink(self.sock_path)
            except OSError:
                pass
        self._write_checkpoint()
        print(self._line(), file=sys.stderr, flush=True)
        if self.stopping and self.mark < len(self.items):
            print("[%s] STOPPED - resume with --resume %s"
                  % (self.name, self.token()), file=sys.stderr, flush=True)
        else:
            try:
                os.unlink(self.ckpt_path)
            except OSError:
                pass
        return self.failures

    @property
    def complete(self):
        return self.mark >= len(self.items)
