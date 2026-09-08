#!/usr/bin/env python3
# PipeWire
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Exercise the real module's occupied Props slot and active-value publication."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def wait_for(process, predicate, description):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if predicate():
            return
        if process.poll() is not None:
            raise RuntimeError(f"process exited before {description}")
        time.sleep(0.01)
    raise RuntimeError(f"timed out waiting for {description}")


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    if len(sys.argv) != 5:
        return 2
    daemon_path, link_path, client_path, plugin_path = (
        str(Path(path).resolve()) for path in sys.argv[1:]
    )
    with tempfile.TemporaryDirectory(prefix="pwao-ndarray-props.") as temporary:
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
        client = None
        with daemon_log.open("w") as daemon_output, client_log.open("w") as client_output:
            daemon = subprocess.Popen(
                [daemon_path], env=environment,
                stdout=daemon_output, stderr=subprocess.STDOUT,
            )
            try:
                wait_for(daemon, lambda: (root / "pipewire-ao-0").is_socket(),
                         "daemon socket")
                client = subprocess.Popen(
                    [client_path, plugin_path], env=environment,
                    stdin=subprocess.PIPE, stdout=client_output,
                    stderr=subprocess.STDOUT, text=True,
                )
                # This warning is emitted only by the module's -EBUSY branch.
                # No source is linked until the actual rejection is observed.
                wait_for(client, lambda:
                         "ndarray graph property update rejected while a transaction is pending"
                         in client_log.read_text(),
                         "the module to reject the occupied Props slot")
                linked = subprocess.run(
                    [link_path, "-w", "ndarray-props-source", "ndarray-props-filter"],
                    env=environment, capture_output=True, text=True, timeout=5,
                )
                if linked.returncode != 0:
                    raise RuntimeError(f"link failed: {linked.stdout}{linked.stderr}")
                wait_for(client, lambda: "STREAMING" in client_log.read_text(),
                         "source to start")
                client.stdin.write("1")
                client.stdin.flush()
                if client.wait(timeout=5) != 0 or "ACTIVE gain=3" not in client_log.read_text():
                    raise RuntimeError("accepted gain was not published after the graph cycle")
                if daemon.poll() is not None:
                    raise RuntimeError("daemon exited during the Props test")
                print("live ndarray filter-chain Props back pressure and publication passed")
                return 0
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                print(error, file=sys.stderr)
                for path in (daemon_log, client_log):
                    print(f"{path.name}:\n{path.read_text()}", file=sys.stderr)
                return 1
            finally:
                stop(client)
                stop(daemon)


if __name__ == "__main__":
    sys.exit(main())
