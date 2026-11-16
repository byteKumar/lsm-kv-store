#!/usr/bin/env python3
"""kill -9 crash test for lsm_server.

Repeats, --kills times:
  1. start lsm_server on the data directory and wait until it listens;
  2. run --clients writer connections. Each writes keys c<id>:<n> with a value
     that encodes the key, and records the write as acknowledged only after the
     server replied OK. Every 10th acknowledged key is then deleted, and the
     delete is recorded once its OK arrives;
  3. after a random 0.3-1.5 s, send SIGKILL to the server (no clean shutdown);
  4. restart it and check every key whose outcome is known:
       acknowledged put (and no delete sent)  -> must return the exact value
       acknowledged delete                    -> must be NOT_FOUND
     A put or delete that was in flight at the kill may land either way; those
     are counted but not judged.

Prints one "RESULT crash_test ..." line. Exit status 1 if any acknowledged
write was lost or any acknowledged delete came back.
"""

import argparse
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time


def start_server(args):
    t0 = time.monotonic()
    proc = subprocess.Popen(
        [args.server, "--dir", args.dir, "--port", str(args.port), "--sync", args.sync,
         "--memtable-kb", str(args.memtable_kb)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    line = proc.stdout.readline()
    if "listening" not in line:
        rest = proc.stdout.read() if proc.poll() is not None else ""
        sys.exit(f"server did not start: {line}{rest}")
    return proc, time.monotonic() - t0


class Conn:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = self.sock.makefile("rwb")

    def call(self, cmd):
        self.f.write((cmd + "\n").encode())
        self.f.flush()
        line = self.f.readline()
        if not line:
            raise ConnectionError("closed")
        return line.decode().rstrip("\n")

    def pipeline(self, cmds):
        self.f.write("".join(c + "\n" for c in cmds).encode())
        self.f.flush()
        out = []
        for _ in cmds:
            line = self.f.readline()
            if not line:
                raise ConnectionError("closed")
            out.append(line.decode().rstrip("\n"))
        return out

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def value_for(key):
    return (key + ":" + "x" * 100)[:100]


class Model:
    """What the database must contain, built only from acknowledged replies."""

    def __init__(self):
        self.lock = threading.Lock()
        self.present = set()   # acknowledged put, no delete sent
        self.absent = set()    # acknowledged delete
        self.unknown = set()   # put or delete in flight when the server died
        self.acked_puts = 0
        self.acked_dels = 0


def writer(cid, start_n, port, model, stop, counters):
    n = start_n
    try:
        conn = Conn(port)
    except OSError:
        return
    try:
        while not stop.is_set():
            key = f"c{cid}:{n:09d}"
            n += 1
            with model.lock:
                model.unknown.add(key)
            if conn.call(f"PUT {key} {value_for(key)}") != "OK":
                raise RuntimeError("bad PUT reply")
            with model.lock:
                model.unknown.discard(key)
                model.present.add(key)
                model.acked_puts += 1
            if n % 10 == 0:
                with model.lock:
                    model.present.discard(key)
                    model.unknown.add(key)
                if conn.call(f"DEL {key}") != "OK":
                    raise RuntimeError("bad DEL reply")
                with model.lock:
                    model.unknown.discard(key)
                    model.absent.add(key)
                    model.acked_dels += 1
    except (OSError, ConnectionError):
        pass  # the server was killed mid-request
    finally:
        counters[cid] = n
        conn.close()


def verify(port, model):
    conn = Conn(port)
    lost, resurrected, wrong = [], [], []
    inflight_present = 0
    try:
        with model.lock:
            present = sorted(model.present)
            absent = sorted(model.absent)
            unknown = sorted(model.unknown)
        for i in range(0, len(present), 2000):
            batch = present[i:i + 2000]
            for key, r in zip(batch, conn.pipeline([f"GET {k}" for k in batch])):
                if r == "NOT_FOUND":
                    lost.append(key)
                elif r != "VALUE " + value_for(key):
                    wrong.append((key, r))
        for i in range(0, len(absent), 2000):
            batch = absent[i:i + 2000]
            for key, r in zip(batch, conn.pipeline([f"GET {k}" for k in batch])):
                if r != "NOT_FOUND":
                    resurrected.append(key)
        for i in range(0, len(unknown), 2000):
            batch = unknown[i:i + 2000]
            for r in conn.pipeline([f"GET {k}" for k in batch]):
                inflight_present += r.startswith("VALUE ")
        stats = conn.call("STATS")
    finally:
        conn.close()
    return lost, resurrected, wrong, inflight_present, len(unknown), stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="build/lsm_server")
    ap.add_argument("--dir", required=True)
    ap.add_argument("--kills", type=int, default=100)
    ap.add_argument("--clients", type=int, default=4)
    ap.add_argument("--sync", default="full", choices=["none", "fsync", "full"])
    ap.add_argument("--memtable-kb", type=int, default=256)
    ap.add_argument("--port", type=int, default=17171)
    ap.add_argument("--min-run", type=float, default=0.3)
    ap.add_argument("--max-run", type=float, default=1.5)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    shutil.rmtree(args.dir, ignore_errors=True)
    model = Model()
    next_n = [0] * args.clients
    totals = {"lost": 0, "resurrected": 0, "wrong": 0}
    recovery_times = []
    first_failure = None
    busy = {"runs_with_flush": 0, "runs_with_compaction": 0, "killed_during_flush": 0,
            "killed_during_compaction": 0, "stats_failed": 0}

    proc, _ = start_server(args)
    for kill in range(1, args.kills + 1):
        stop = threading.Event()
        threads = [threading.Thread(target=writer, args=(c, next_n[c], args.port, model, stop, next_n))
                   for c in range(args.clients)]
        for t in threads:
            t.start()
        time.sleep(rng.uniform(args.min_run, args.max_run))
        # What the server is doing right before the kill (a few microseconds
        # earlier), so we can say how many kills hit a flush or compaction.
        try:
            c = Conn(args.port)
            pre = dict(kv.split("=") for kv in c.call("STATS").split()[1:])
            c.close()
            busy["runs_with_flush"] += int(pre["flushes"]) > 0
            busy["runs_with_compaction"] += int(pre["compactions"]) > 0
            busy["killed_during_flush"] += pre["flush_running"] == "1"
            busy["killed_during_compaction"] += pre["compaction_running"] == "1"
        except (OSError, ConnectionError, ValueError, KeyError):
            busy["stats_failed"] += 1
        os.kill(proc.pid, signal.SIGKILL)  # no warning, no cleanup
        proc.wait()
        stop.set()
        for t in threads:
            t.join()

        proc, rec = start_server(args)
        recovery_times.append(rec)
        lost, resurrected, wrong, inflight_present, inflight, stats = verify(args.port, model)
        totals["lost"] += len(lost)
        totals["resurrected"] += len(resurrected)
        totals["wrong"] += len(wrong)
        if (lost or resurrected or wrong) and first_failure is None:
            first_failure = (kill, lost[:5], resurrected[:5], wrong[:3])
        print(f"kill {kill:3d}: acked_puts={model.acked_puts} acked_dels={model.acked_dels} "
              f"lost={len(lost)} resurrected={len(resurrected)} wrong={len(wrong)} "
              f"inflight={inflight} (present after restart: {inflight_present}) "
              f"recovery={rec * 1000:.0f}ms | {stats}", flush=True)
        # In-flight keys stay "unknown" forever; later checks skip them.

    proc.send_signal(signal.SIGTERM)
    proc.wait()
    recovery_times.sort()
    print(f"RESULT crash_test sync={args.sync} kills={args.kills} clients={args.clients} "
          f"acked_puts={model.acked_puts} acked_dels={model.acked_dels} "
          f"keys_checked_last={len(model.present) + len(model.absent)} "
          f"lost_acked_writes={totals['lost']} resurrected_deletes={totals['resurrected']} "
          f"wrong_values={totals['wrong']} "
          f"recovery_ms_p50={recovery_times[len(recovery_times) // 2] * 1000:.0f} "
          f"recovery_ms_max={recovery_times[-1] * 1000:.0f} "
          + " ".join(f"{k}={v}" for k, v in busy.items()))
    if first_failure:
        print("FIRST FAILURE (kill, lost, resurrected, wrong):", first_failure)
    shutil.rmtree(args.dir, ignore_errors=True)
    return 1 if first_failure else 0


if __name__ == "__main__":
    sys.exit(main())
