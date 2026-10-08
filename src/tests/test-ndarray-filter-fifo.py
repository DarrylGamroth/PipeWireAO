#!/usr/bin/env python3
# PipeWire
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Observe a finite native FIFO burst and output-return-driven backlog recovery."""

import csv
import importlib.util
import io
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location(
    "ndarray_remote", Path(__file__).with_name("test-ndarray-filter-remote.py"))
remote = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(remote)


def check_trace(directory, callbacks):
    paths = list(directory.glob("ndarray-filter-*.csv"))
    if len(paths) != 1:
        raise remote.TestFailure("expected one native helper trace")
    text = paths[0].read_text()
    print("--- helper trace\n" + text, flush=True)
    lines = text.splitlines()
    if not lines or not lines[0].endswith(" omitted=0"):
        raise remote.TestFailure("native helper trace is incomplete")
    records = list(csv.DictReader(io.StringIO("\n".join(lines[1:]))))
    expected = [(100 + packet // 4, (packet % 4) * 2) for packet in range(8)]
    def events(event):
        return [row for row in records if row["event"] == event]
    def identities(rows):
        return [(int(row["frame"]), int(row["offset"])) for row in rows]
    given, ended, recycled, dequeued = map(events, ("G", "E", "R", "D"))
    if any(identities(rows) != expected[:callbacks] for rows in (given, ended, recycled)):
        raise remote.TestFailure("FIFO callback or recycle order differs from original rows")
    if identities(dequeued) != expected[:min(callbacks + 1, 8)]:
        raise remote.TestFailure("queued next input was not preserved")
    if any(int(row["result"]) != 0 for row in records):
        raise remote.TestFailure("native helper trace records an operation failure")
    for index in range(callbacks):
        if not given[index]["activation"] == ended[index]["activation"] == recycled[index]["activation"]:
            raise remote.TestFailure("one input crossed callback activation boundaries")
    for index in range(1, len(dequeued)):
        if dequeued[index]["activation"] != ended[index - 1]["activation"]:
            raise remote.TestFailure("next original row was not already queued at callback end")
    if identities(events("O")) != expected[1:callbacks]:
        raise remote.TestFailure("OUTPUT_UNAVAILABLE output was published or output order changed")


def main():
    if len(sys.argv) not in (5, 6) or (len(sys.argv) == 6 and sys.argv[5] != "--expect-stall"):
        return 2
    daemon_path, link_path, filter_path, client_path = map(Path, sys.argv[1:5])
    expect_stall = len(sys.argv) == 6
    temporary = Path(tempfile.mkdtemp(prefix="pwao-native-fifo."))
    runtime = temporary / "run"
    runtime.mkdir(mode=0o700)
    trace = temporary / "trace"
    trace.mkdir()
    environment = os.environ.copy()
    environment.update({"PIPEWIREAO_RUNTIME_DIR": str(runtime),
        "PIPEWIREAO_REMOTE": "pipewire-ao-0", "PIPEWIREAO_LOG_SYSTEMD": "false",
        "PW_NDARRAY_FILTER_TRACE_DIR": str(trace)})
    logs = {name: temporary / f"{name}.log" for name in ("daemon", "filter", "client")}
    handles = {name: path.open("w+") for name, path in logs.items()}
    processes = []
    try:
        daemon = subprocess.Popen([str(daemon_path), "-P",
            "{ context.data-loops = [ { loop.name=daemon-event loop.class=data.rt loop.idle=eventfd } ] }"],
            env=environment, stdout=handles["daemon"], stderr=subprocess.STDOUT)
        processes.append(daemon)
        remote.wait_for(lambda: (runtime / "pipewire-ao-0").is_socket(), 5, "native core")
        filt = subprocess.Popen([str(filter_path)], env=environment, stdin=subprocess.PIPE, text=True,
            stdout=handles["filter"], stderr=subprocess.STDOUT)
        processes.append(filt)
        client = subprocess.Popen([str(client_path)], env=environment, stdin=subprocess.PIPE, text=True,
            stdout=handles["client"], stderr=subprocess.STDOUT)
        processes.append(client)
        remote.wait_for_text(filt, logs["filter"], "READY", 5)
        remote.wait_for_text(client, logs["client"], "READY", 5)
        remote.link_nodes(link_path, environment, "ndarray-fifo-source", "ndarray-fifo-filter")
        remote.link_nodes(link_path, environment, "ndarray-fifo-filter", "ndarray-fifo-sink")
        remote.wait_for_text(filt, logs["filter"], "PREPARED", 5)
        remote.wait_for_text(client, logs["client"], "STREAMING", 5)
        filt.stdin.write("H")
        filt.stdin.flush()
        remote.wait_for_text(filt, logs["filter"], "HELD_OUTPUT_POOL", 5)
        client.stdin.write("B")
        client.stdin.flush()
        remote.wait_for_text(client, logs["client"], "BURST_DONE produced=8", 5)
        time.sleep(0.1)
        initial = remote.read_log(logs["filter"])
        if initial.count("CALLBACK ") != 0:
            raise remote.TestFailure("held negotiated outputs did not starve the native output pool: " + initial)
        filt.stdin.write("R")
        filt.stdin.flush()
        if expect_stall:
            remote.wait_for_text(filt, logs["filter"], "SUPPRESSED_BACKLOG_REQUEST", 5)
            time.sleep(1)
            before = remote.read_log(logs["filter"]).count("CALLBACK ")
            time.sleep(0.1)
            after = remote.read_log(logs["filter"]).count("CALLBACK ")
            if before != after or not 1 <= after < 8 or filt.poll() is not None:
                raise remote.TestFailure("disabled backlog did not leave stable live backlog")
        else:
            remote.wait_for_text(client, logs["client"], "RESULT produced=8 received=7", 5)
        # Stop the helper before removing the source pool: removal would correctly
        # invalidate retained inputs and obscure the negative control's outcome.
        filt.send_signal(signal.SIGTERM)
        helper_status = filt.wait(timeout=5)
        text = remote.read_log(logs["filter"])
        if expect_stall:
            if helper_status != 1 or f"SUMMARY callbacks={after} retained={int(after >= 2)} error=0" not in text:
                raise remote.TestFailure("disabled-backlog failure was not a clean live stall: " + text)
        elif helper_status != 0 or "RESULT callbacks=8 retained=1" not in text:
            raise remote.TestFailure("native FIFO processing was incomplete: " + text)
        if not expect_stall:
            client.stdin.write("Q")
            client.stdin.flush()
            if client.wait(timeout=5) != 0:
                raise remote.TestFailure("native endpoint client failed")
        sources = [line for line in remote.read_log(logs["client"]).splitlines() if line.startswith("SOURCE ")]
        if sources != [f"SOURCE {packet}" for packet in range(1, 9)]:
            raise remote.TestFailure("finite source published unexpected original inputs")
        check_trace(trace, after if expect_stall else 8)
        if daemon.poll() is not None:
            raise remote.TestFailure("native core exited during FIFO proof")
        print("native FIFO disabled-backlog stall passed" if expect_stall else
              "native FIFO burst and output-capacity recovery passed")
        return 0
    except (remote.TestFailure, subprocess.TimeoutExpired) as error:
        print(f"native FIFO proof failed: {error}", file=sys.stderr)
        return 1
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for handle in handles.values():
            handle.close()
        for name, path in logs.items():
            print(f"--- {name}.log\n{remote.read_log(path)}", flush=True)
        print(f"EVIDENCE_DIRECTORY={temporary}", flush=True)
        # Retain the bounded original logs and trace for qualification review.


if __name__ == "__main__":
    sys.exit(main())
