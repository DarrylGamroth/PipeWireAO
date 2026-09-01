#!/usr/bin/env python3
# PipeWire
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Prove bounded Parameter Port preparation through connected buffers."""

import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


class TestFailure(RuntimeError):
    pass


def read_log(path):
    return path.read_text(encoding="utf-8") if path.exists() else ""


def wait_for(predicate, timeout, description):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)
    raise TestFailure(f"timed out waiting for {description}")


def wait_for_text(process, path, expected, timeout):
    def observed():
        contents = read_log(path)
        if expected in contents:
            return contents
        if process.poll() is not None:
            raise TestFailure(
                f"{path.stem} exited before {expected!r} "
                f"({process.returncode}):\n{contents}"
            )
        return False

    return wait_for(observed, timeout, f"{path.stem} to report {expected!r}")


def main():
    if len(sys.argv) != 5:
        return 2
    paths = {
        "daemon": Path(sys.argv[1]).resolve(),
        "link": Path(sys.argv[2]).resolve(),
        "filter": Path(sys.argv[3]).resolve(),
        "client": Path(sys.argv[4]).resolve(),
    }
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise TestFailure("missing build artifacts: " + ", ".join(missing))

    temporary = Path(tempfile.mkdtemp(prefix="pwao-ndarray-parameter."))
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
    logs = {
        name: temporary / f"{name}.log"
        for name in ("daemon", "filter", "client")
    }
    handles = {
        name: path.open("w+", encoding="utf-8") for name, path in logs.items()
    }
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
        stdout=handles["daemon"],
        stderr=subprocess.STDOUT,
    )
    filter_process = None
    client_process = None
    try:
        wait_for(
            lambda: (runtime / "pipewire-ao-0").is_socket(),
            5,
            "daemon socket",
        )
        filter_process = subprocess.Popen(
            [str(paths["filter"])],
            env=environment,
            text=True,
            stdout=handles["filter"],
            stderr=subprocess.STDOUT,
        )
        client_process = subprocess.Popen(
            [str(paths["client"])],
            env=environment,
            text=True,
            stdin=subprocess.PIPE,
            stdout=handles["client"],
            stderr=subprocess.STDOUT,
        )
        wait_for_text(filter_process, logs["filter"], "READY", 5)
        wait_for_text(client_process, logs["client"], "READY", 5)
        result = subprocess.run(
            [
                str(paths["link"]),
                "-w",
                "ndarray-parameter-source",
                "ndarray-parameter-filter",
            ],
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=5,
            check=False,
        )
        if result.returncode != 0:
            raise TestFailure(
                f"could not link Parameter Port ({result.returncode}):\n"
                f"{result.stdout}"
            )
        wait_for_text(filter_process, logs["filter"], "PREPARED", 5)
        wait_for_text(client_process, logs["client"], "STREAMING", 5)

        assert client_process.stdin is not None
        client_process.stdin.write("0")
        client_process.stdin.flush()
        wait_for_text(client_process, logs["client"], "SOURCE 1 absent=1", 5)

        client_process.stdin.write("1")
        client_process.stdin.flush()
        wait_for_text(client_process, logs["client"], "SOURCE 2 value=5", 5)
        wait_for_text(filter_process, logs["filter"], "PARAMETER_BUSY", 5)
        if "PARAMETER_READY" in read_log(logs["filter"]):
            raise TestFailure("busy Parameter was retried without a later cycle")

        client_process.stdin.write("2")
        client_process.stdin.flush()
        wait_for_text(client_process, logs["client"], "SOURCE 3 value=9", 5)
        wait_for_text(filter_process, logs["filter"], "PARAMETER_READY", 5)

        client_process.stdin.write("q")
        client_process.stdin.flush()
        filter_process.send_signal(signal.SIGTERM)
        client_result = client_process.wait(timeout=5)
        filter_result = filter_process.wait(timeout=5)
        client_text = read_log(logs["client"])
        filter_text = read_log(logs["filter"])
        if client_result != 0 or "RESULT produced=3 absent=1" not in client_text:
            raise TestFailure(
                f"Parameter source failed ({client_result}):\n{client_text}"
            )
        if filter_result != 0 or "parameter=2 retained=1" not in filter_text:
            raise TestFailure(
                f"Parameter filter failed ({filter_result}):\n{filter_text}"
            )
        if daemon.poll() is not None:
            raise TestFailure("daemon exited during Parameter Port proof")
        print("connected ndarray Parameter Port handoff passed")
        return 0
    except (subprocess.TimeoutExpired, TestFailure) as error:
        print(f"connected ndarray Parameter Port test failed: {error}", file=sys.stderr)
        for name, path in logs.items():
            contents = read_log(path)
            if contents:
                print(f"--- {name}.log\n{contents}", file=sys.stderr)
        return 1
    finally:
        for process in (client_process, filter_process):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
        if daemon.poll() is None:
            daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
        for handle in handles.values():
            handle.close()
        shutil.rmtree(temporary)


if __name__ == "__main__":
    sys.exit(main())
