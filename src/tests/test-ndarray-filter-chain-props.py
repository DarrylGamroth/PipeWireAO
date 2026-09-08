#!/usr/bin/env python3
# PipeWire
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Exercise live property publication, snapshot retry, and pending teardown."""

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


def run_case(daemon_path, link_path, client_path, plugin_path, mode, filter_name):
    with tempfile.TemporaryDirectory(prefix=f"pwao-ndarray-props-{mode}.") as temporary:
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
                    [client_path, plugin_path, mode], env=environment,
                    stdin=subprocess.PIPE, stdout=client_output,
                    stderr=subprocess.STDOUT, text=True,
                )
                if mode in ("back-pressure", "teardown"):
                    # This warning is emitted only by the module's -EBUSY branch.
                    # No source is linked until the actual rejection is observed.
                    wait_for(client, lambda:
                             "ndarray graph property update rejected while a transaction is pending"
                             in client_log.read_text(),
                             "the module to reject the occupied Props slot")
                else:
                    wait_for(client, lambda:
                             "INITIAL value=0 mirror=0" in client_log.read_text(),
                             "the initial stable property snapshot")
                linked = subprocess.run(
                    [link_path, "-w", "ndarray-props-source", filter_name],
                    env=environment, capture_output=True, text=True, timeout=5,
                )
                if linked.returncode != 0:
                    raise RuntimeError(f"link failed: {linked.stdout}{linked.stderr}")
                wait_for(client, lambda: "STREAMING" in client_log.read_text(),
                         "source to start")
                client.stdin.write("1")
                client.stdin.flush()
                if mode == "back-pressure":
                    if (client.wait(timeout=5) != 0 or
                            "ACTIVE gain=3" not in client_log.read_text()):
                        raise RuntimeError(
                            "accepted gain was not published after the graph cycle")
                elif mode == "unstable-snapshot":
                    wait_for(client, lambda:
                             "ODD_SNAPSHOT revision=1" in client_log.read_text(),
                             "the module to reject its odd property snapshot")
                    if "STABLE value=2 mirror=20" in client_log.read_text():
                        raise RuntimeError("odd property snapshot was published")
                    client.stdin.write("1")
                    client.stdin.flush()
                    if (client.wait(timeout=5) != 0 or
                            "STABLE value=2 mirror=20" not in client_log.read_text()):
                        raise RuntimeError(
                            "stable property snapshot was not published after recovery")
                else:
                    expected = "TEARDOWN pending event removed; core roundtrip completed"
                    if client.wait(timeout=7) != 0 or expected not in client_log.read_text():
                        raise RuntimeError(
                            "module teardown did not remove the pending notification")
                if daemon.poll() is not None:
                    raise RuntimeError("daemon exited during the Props test")
                print(f"live ndarray filter-chain Props {mode} passed")
                return 0
            except (BrokenPipeError, RuntimeError,
                    subprocess.TimeoutExpired) as error:
                print(error, file=sys.stderr)
                for path in (daemon_log, client_log):
                    print(f"{path.name}:\n{path.read_text()}", file=sys.stderr)
                return 1
            finally:
                stop(client)
                stop(daemon)


def main():
    if len(sys.argv) not in (6, 7) or (len(sys.argv) == 7 and sys.argv[6] != "teardown"):
        return 2
    daemon_path, link_path, client_path, example_plugin, unstable_plugin = (
        str(Path(path).resolve()) for path in sys.argv[1:6]
    )
    if len(sys.argv) == 7:
        return run_case(daemon_path, link_path, client_path, example_plugin,
                        "teardown", "ndarray-props-filter")
    if run_case(daemon_path, link_path, client_path, example_plugin,
                "back-pressure", "ndarray-props-filter") != 0:
        return 1
    return run_case(daemon_path, link_path, client_path, unstable_plugin,
                    "unstable-snapshot", "ndarray-unstable-props-filter")


if __name__ == "__main__":
    sys.exit(main())
