#!/usr/bin/env python3
# PipeWire
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Exercise exported polling nodes through an isolated PipeWireAO daemon."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


class TestFailure(RuntimeError):
    pass


def wait_for(predicate, timeout, description):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)
    raise TestFailure(f"timed out waiting for {description}")


def run_case(paths, environment, directory, name, source_idle, sink_idle, hold=False):
    log_path = directory / f"{name}.log"
    with log_path.open("w+", encoding="utf-8") as output:
        arguments = [
            str(paths["client"]),
            name,
            source_idle,
            sink_idle,
            "100000" if hold else "128",
        ]
        if hold:
            arguments.append("hold")
        client = subprocess.Popen(
            arguments,
            env=environment,
            text=True,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
        try:
            def link_streams():
                if client.poll() is not None:
                    output.flush()
                    text = log_path.read_text(encoding="utf-8")
                    raise TestFailure(
                        f"{name} exited before linking ({client.returncode}):\n{text}"
                    )
                try:
                    linked = subprocess.run(
                        [
                            str(paths["link"]),
                            "-w",
                            f"polling-export-source-{name}",
                            f"polling-export-sink-{name}",
                        ],
                        env=environment,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        timeout=1,
                        check=False,
                    )
                except subprocess.TimeoutExpired:
                    return False
                return linked.returncode == 0

            wait_for(link_streams, 5, f"{name} exported stream link")
            if hold:
                def buffer_held():
                    output.flush()
                    return "HOLDING " in log_path.read_text(encoding="utf-8")

                wait_for(buffer_held, 5, f"{name} retained buffer")
            try:
                result = client.wait(timeout=10)
            except subprocess.TimeoutExpired as error:
                client.kill()
                client.wait()
                raise TestFailure(f"{name} client timed out") from error
            output.flush()
            text = log_path.read_text(encoding="utf-8")
            if result != 0 or "RESULT " not in text:
                raise TestFailure(f"{name} failed ({result}):\n{text}")
            return text
        finally:
            if client.poll() is None:
                client.kill()
                client.wait()


def main():
    if len(sys.argv) != 4:
        return 2
    paths = {
        "daemon": Path(sys.argv[1]).resolve(),
        "link": Path(sys.argv[2]).resolve(),
        "client": Path(sys.argv[3]).resolve(),
    }
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise TestFailure("missing build artifacts: " + ", ".join(missing))
    temporary = Path(tempfile.mkdtemp(prefix="pwao-polling-remote."))
    runtime = temporary / "run"
    runtime.mkdir()
    environment = os.environ.copy()
    environment.update(
        {
            "PIPEWIREAO_RUNTIME_DIR": str(runtime),
            "PIPEWIREAO_REMOTE": "pipewire-ao-0",
            "PIPEWIREAO_LOG_SYSTEMD": "false",
        }
    )
    daemon_log_path = temporary / "daemon.log"
    daemon_log = daemon_log_path.open("w+", encoding="utf-8")
    daemon = subprocess.Popen(
        [
            str(paths["daemon"]),
            "-P",
            "{ context.data-loops = [ "
            "{ loop.name=daemon-event loop.class=data.rt loop.idle=eventfd } "
            "] }",
        ],
        env=environment,
        text=True,
        stdout=daemon_log,
        stderr=subprocess.STDOUT,
    )
    try:
        wait_for(
            lambda: (runtime / "pipewire-ao-0").is_socket(),
            5,
            "daemon socket",
        )
        run_case(paths, environment, temporary, "poll-event", "busy-spin", "eventfd")
        run_case(paths, environment, temporary, "event-poll", "eventfd", "busy-spin")
        run_case(
            paths,
            environment,
            temporary,
            "active-teardown",
            "busy-spin",
            "eventfd",
            hold=True,
        )
        run_case(paths, environment, temporary, "reconnect", "busy-spin", "eventfd")
        if daemon.poll() is not None:
            raise TestFailure("daemon exited during cross-process qualification")
        print("cross-process polling matrix passed")
        return 0
    except TestFailure as error:
        print(f"cross-process polling test failed: {error}", file=sys.stderr)
        for log_path in sorted(temporary.glob("*.log")):
            contents = log_path.read_text(encoding="utf-8")
            if contents:
                print(f"--- {log_path.name}\n{contents}", file=sys.stderr)
        return 1
    finally:
        if daemon.poll() is None:
            daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
        daemon_log.close()
        shutil.rmtree(temporary)


if __name__ == "__main__":
    sys.exit(main())
