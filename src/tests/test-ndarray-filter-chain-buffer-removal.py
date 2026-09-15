#!/usr/bin/env python3
# PipeWire
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Replace a live filter-chain output pool while a required input is absent."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def wait_for(process, log, expected):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if expected in log.read_text():
            return
        if process.poll() is not None:
            raise RuntimeError(f"client exited before {expected}")
        time.sleep(0.01)
    raise RuntimeError(f"timed out waiting for {expected}")


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def link(link_path, environment, output, source, sink):
    result = subprocess.run(
        [link_path, "-w", source, sink], env=environment,
        capture_output=True, text=True, timeout=5,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"link {source} -> {sink} failed: {result.stdout}{result.stderr}")
    output.write(result.stdout + result.stderr)
    output.flush()


def main():
    if len(sys.argv) != 5:
        return 2
    daemon_path, link_path, client_path, plugin_path = (
        str(Path(path).resolve()) for path in sys.argv[1:]
    )
    with tempfile.TemporaryDirectory(prefix="pwao-ndarray-buffer-removal.") as temporary:
        root = Path(temporary)
        environment = os.environ.copy()
        environment.update({
            "PIPEWIREAO_RUNTIME_DIR": temporary,
            "PIPEWIREAO_REMOTE": "pipewire-ao-0",
            "PIPEWIREAO_LOG_SYSTEMD": "false",
            "PIPEWIREAO_DEBUG": "2",
        })
        daemon_log = root / "daemon.log"
        client_log = root / "client.log"
        link_log = root / "link.log"
        daemon = client = None
        try:
            with (daemon_log.open("w") as daemon_output,
                  client_log.open("w") as client_output,
                  link_log.open("w") as link_output):
                daemon = subprocess.Popen(
                    [daemon_path], env=environment, stdout=daemon_output,
                    stderr=subprocess.STDOUT,
                )
                deadline = time.monotonic() + 5
                while not (root / "pipewire-ao-0").is_socket():
                    if daemon.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("daemon socket did not appear")
                    time.sleep(0.01)
                client = subprocess.Popen(
                    [client_path, plugin_path], env=environment,
                    stdin=subprocess.PIPE, stdout=client_output,
                    stderr=subprocess.STDOUT, text=True,
                )
                wait_for(client, client_log, "READY")
                link(link_path, environment, link_output,
                     "ndarray-buffer-removal-source", "ndarray-buffer-removal-filter")
                link(link_path, environment, link_output,
                     "ndarray-buffer-removal-filter", "ndarray-buffer-removal-sink")
                client.stdin.write("a")
                client.stdin.flush()
                wait_for(client, client_log, "INPUT_ABSENT")
                # Let the selected driver's follower call retain its dequeued
                # output before its connected sink tears the pool down.
                time.sleep(0.1)
                client.stdin.write("d")
                client.stdin.flush()
                wait_for(client, client_log, "OUTPUT_POOL_REMOVED")
                client.stdin.write("n")
                client.stdin.flush()
                wait_for(client, client_log, "OUTPUT_POOL_REPLACED")
                link(link_path, environment, link_output,
                     "ndarray-buffer-removal-filter", "ndarray-buffer-removal-sink")
                wait_for(client, client_log, "SINK_REPLACEMENT_STREAMING")
                client.stdin.write("r")
                client.stdin.flush()
                if (client.wait(timeout=7) != 0 or
                        "RESULT output=4 after output-pool replacement"
                        not in client_log.read_text()):
                    raise RuntimeError(
                        "resumed graph did not safely publish through the replacement pool "
                        f"(status {client.returncode})")
                if daemon.poll() is not None:
                    raise RuntimeError("daemon exited during buffer-pool replacement")
        except (BrokenPipeError, RuntimeError, subprocess.TimeoutExpired) as error:
            print(error, file=sys.stderr)
            for path in (daemon_log, client_log, link_log):
                if path.exists():
                    print(f"{path.name}:\n{path.read_text()}", file=sys.stderr)
            return 1
        finally:
            stop(client)
            stop(daemon)
    print("live ndarray filter-chain output-pool replacement passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
