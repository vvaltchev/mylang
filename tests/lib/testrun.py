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
#              runs directory (tests/testctl list | status | failures |
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
# A tool whose work is NOT a list of independent items - sequential
# phases, a shrink with no fixed total, a loop that must stay serial -
# uses a `Monitor` instead: the same socket, heartbeat line and status
# fields, fed by the tool's own `advance()` / `phase()` calls, without
# the resume machinery. EVERY test tool that can run for more than a
# minute serves one or the other, so `tests/testctl` (no arguments)
# lists everything running with a percentage.
#
# THE STATUS FIELDS both serve (the `status` action): name, pid, phase,
# done, total, `percent` (0-100, the obvious one), elapsed_s, eta_s and
# `eta` (human), rate_per_s, failures, plus each kind's extras.
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


def fmt_secs(secs):
    if secs is None:
        return "?"
    secs = int(secs)
    if secs >= 3600:
        return "%dh%02dm" % (secs // 3600, secs % 3600 // 60)
    return "%dm%02ds" % divmod(secs, 60)


def percent_of(done, total):
    return round(100.0 * done / total, 1) if total else 0.0


def serve_socket(sock_path, handle):
    """Listen on `sock_path`; each request line (JSON) is answered with
    handle(request) (JSON). Returns the listening socket (close it to
    stop)."""
    try:
        os.unlink(sock_path)
    except OSError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock_path)
    srv.listen(8)

    def client(conn):
        with conn:
            f = conn.makefile("rw")
            for line in f:
                try:
                    resp = handle(json.loads(line))
                except (ValueError, TypeError) as e:
                    resp = {"error": str(e)}
                f.write(json.dumps(resp) + "\n")
                f.flush()

    def accept():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=client, args=(conn,),
                             daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    return srv


class Monitor:
    """Progress and status for a tool that is not a `Run` (see the header).

        mon = Monitor("int_run", total=len(units), phase="units")
        with mon:
            for u in units:
                mon.set_current(u)
                ...
                mon.advance()
            mon.phase("census", total=len(corpus))

    `percent_fn`, when given, overrides done/total (a time-budgeted
    shrink knows its progress only as elapsed / budget)."""

    def __init__(self, name, total=0, phase=None, heartbeat=60.0,
                 percent_fn=None, pid=None):
        self.name = name
        # the pid shown and used in the socket name: the tool's own, or
        # (tests/testmon) the shell tool it watches
        self.pid = pid or os.getpid()
        self.total = total
        self.done = 0
        self.failures = 0
        self.cur_phase = phase
        self.current = None
        self.extra = {}
        self.heartbeat = heartbeat
        self.percent_fn = percent_fn
        self.lock = threading.Lock()
        self.t0 = self.tp = time.time()
        self.sock_path = os.path.join(
            runs_dir(), "%s-%d.sock" % (name, self.pid))
        self.srv = None
        self.stop_beat = threading.Event()

    def __enter__(self):
        self.srv = serve_socket(self.sock_path, self._handle)
        print("[%s] started; control socket %s" % (self.name,
                                                   self.sock_path),
              file=sys.stderr, flush=True)

        def beat():
            while not self.stop_beat.wait(self.heartbeat):
                print(self.line(), file=sys.stderr, flush=True)

        threading.Thread(target=beat, daemon=True).start()
        return self

    def __exit__(self, *exc):
        self.stop_beat.set()
        if self.srv:
            self.srv.close()
        try:
            os.unlink(self.sock_path)
        except OSError:
            pass
        return False

    def advance(self, n=1, failed=False):
        with self.lock:
            self.done += n
            if failed:
                self.failures += 1

    def phase(self, name, total=None):
        """Enter a new phase; its own done/total restart from 0."""
        with self.lock:
            self.cur_phase = name
            self.done = 0
            self.tp = time.time()
            if total is not None:
                self.total = total

    def set_total(self, total):
        with self.lock:
            self.total = total

    def set_current(self, what):
        with self.lock:
            self.current = str(what)

    def set_extra(self, **kw):
        with self.lock:
            self.extra.update(kw)

    def status(self):
        with self.lock:
            now = time.time()
            el = now - self.tp
            rate = self.done / el if el > 0 else 0.0
            pct = (self.percent_fn() if self.percent_fn
                   else percent_of(self.done, self.total))
            left = self.total - self.done
            eta = (round(left / rate, 1)
                   if rate > 0 and left >= 0 and not self.percent_fn
                   else (round(el * (100 - pct) / pct, 1)
                         if self.percent_fn and pct > 0 else None))
            st = {"name": self.name, "pid": self.pid,
                  "phase": self.cur_phase, "percent": round(pct, 1),
                  "done": self.done, "total": self.total,
                  "elapsed_s": round(now - self.t0, 1),
                  "rate_per_s": round(rate, 2), "eta_s": eta,
                  "eta": fmt_secs(eta), "failures": self.failures,
                  "current": self.current}
            st.update(self.extra)
            return st

    def line(self):
        s = self.status()
        return ("[%s] %s%.1f%% (%d/%d), ETA %s, %d failure(s)%s"
                % (self.name, (s["phase"] + ": ") if s["phase"] else "",
                   s["percent"], s["done"], s["total"], s["eta"],
                   s["failures"],
                   (", now " + s["current"]) if s["current"] else ""))

    def _handle(self, req):
        if req.get("action") == "status":
            return self.status()
        return {"error": "%s supports only `status` (it is a Monitor, "
                         "not a resumable Run)" % self.name}


def parse_token(token):
    """`<mark>@<fingerprint>` -> (mark, fingerprint)."""
    mark, _, fp = token.partition("@")
    return int(mark), fp


class Run:
    """Drives `work(i, item) -> failure-text-or-None` over `items`.

    `name` labels the run (tests/testctl list), `fp` is the plan
    fingerprint, `resume` an optional token. `describe(item)` renders an
    item for status displays."""

    def __init__(self, name, items, work, fp, jobs, resume=None,
                 heartbeat=60.0, describe=str, window_per_job=4,
                 phase=None):
        self.name = name
        self.phase = phase
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
            eta = round(left / rate, 1) if rate > 0 else None
            done = self.mark + len(self.done)
            return {
                "name": self.name, "pid": os.getpid(),
                "phase": self.phase,
                "percent": percent_of(done, len(self.items)),
                "eta": fmt_secs(eta),
                "total": len(self.items), "mark": self.mark,
                "done": done,
                "this_run_done": self.ndone,
                "elapsed_s": round(el, 1),
                "rate_per_s": round(rate, 2),
                "eta_s": eta,
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
        return ("[%s] %s%.1f%% (%d/%d), %.1f/s, ETA %s, %d failure(s), "
                "resume %s" % (self.name,
                               (s["phase"] + ": ") if s["phase"] else "",
                               s["percent"], s["done"], s["total"],
                               s["rate_per_s"], s["eta"], s["failures"],
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
