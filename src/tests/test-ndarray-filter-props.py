#!/usr/bin/env python3
# PipeWire
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Verify the ndarray-filter control surface through a remote Node proxy."""

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


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def main():
    if len(sys.argv) != 4:
        return 2
    paths = {
        "daemon": Path(sys.argv[1]).resolve(),
        "filter": Path(sys.argv[2]).resolve(),
        "client": Path(sys.argv[3]).resolve(),
    }
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise TestFailure("missing build artifacts: " + ", ".join(missing))

    temporary = Path(tempfile.mkdtemp(prefix="pwao-ndarray-props."))
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
    logs = {name: temporary / f"{name}.log" for name in paths}
    handles = {name: path.open("w+", encoding="utf-8")
               for name, path in logs.items()}
    daemon = subprocess.Popen([str(paths["daemon"])], env=environment,
                              text=True, stdout=handles["daemon"],
                              stderr=subprocess.STDOUT)
    filter_process = None
    client_process = None
    try:
        wait_for(lambda: (runtime / "pipewire-ao-0").is_socket(), 5,
                 "daemon socket")
        filter_process = subprocess.Popen([str(paths["filter"])],
                                          env=environment, text=True,
                                          stdout=handles["filter"],
                                          stderr=subprocess.STDOUT)
        wait_for_text(filter_process, logs["filter"], "READY", 5)
        client_process = subprocess.Popen([str(paths["client"])],
                                          env=environment, text=True,
                                          stdout=handles["client"],
                                          stderr=subprocess.STDOUT)
        client_result = client_process.wait(timeout=10)
        client_text = read_log(logs["client"])
        if client_result != 0 or (
            "RESULT public Props surface retained across all updates"
            not in client_text
        ):
            raise TestFailure(f"client failed ({client_result}):\n{client_text}")
        filter_process.send_signal(signal.SIGTERM)
        filter_result = filter_process.wait(timeout=5)
        filter_text = read_log(logs["filter"])
        if filter_result != 0 or "RESULT owner-updates=1 resets=1" not in filter_text:
            raise TestFailure(f"filter failed ({filter_result}):\n{filter_text}")
        if daemon.poll() is not None:
            raise TestFailure("daemon exited during public Props proof")
        print("connected ndarray-filter Props surface passed")
        return 0
    except (subprocess.TimeoutExpired, TestFailure) as error:
        print(f"connected ndarray-filter Props test failed: {error}", file=sys.stderr)
        for name, path in logs.items():
            contents = read_log(path)
            if contents:
                print(f"--- {name}.log\n{contents}", file=sys.stderr)
        return 1
    finally:
        stop(client_process)
        stop(filter_process)
        stop(daemon)
        for handle in handles.values():
            handle.close()
        shutil.rmtree(temporary)


if __name__ == "__main__":
    sys.exit(main())
