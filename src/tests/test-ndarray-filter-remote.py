#!/usr/bin/env python3
# PipeWire
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Prove conditional ndarray output retention through connected buffers."""

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


def wait_for_text(process, path, text, timeout):
    def observed():
        contents = read_log(path)
        if text in contents:
            return contents
        if process.poll() is not None:
            raise TestFailure(
                f"{path.stem} exited before {text!r} "
                f"({process.returncode}):\n{contents}"
            )
        return False

    return wait_for(observed, timeout, f"{path.stem} to report {text!r}")


def link_nodes(link, environment, source, sink):
    result = subprocess.run(
        [str(link), "-w", source, sink],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=5,
        check=False,
    )
    if result.returncode != 0:
        raise TestFailure(
            f"could not link {source} to {sink} ({result.returncode}):\n"
            f"{result.stdout}"
        )


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

    temporary = Path(tempfile.mkdtemp(prefix="pwao-ndarray-retention."))
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
        name: path.open("w+", encoding="utf-8")
        for name, path in logs.items()
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
        link_nodes(
            paths["link"],
            environment,
            "ndarray-retention-source",
            "ndarray-retention-filter",
        )
        link_nodes(
            paths["link"],
            environment,
            "ndarray-retention-filter",
            "ndarray-retention-sink",
        )
        wait_for_text(filter_process, logs["filter"], "PREPARED", 5)
        wait_for_text(client_process, logs["client"], "STREAMING", 5)

        assert client_process.stdin is not None
        client_process.stdin.write("1")
        client_process.stdin.flush()
        wait_for_text(filter_process, logs["filter"], "DEFERRED", 5)
        client_text = read_log(logs["client"])
        if client_process.poll() is not None:
            raise TestFailure(
                f"endpoint client exited after the deferred callback "
                f"({client_process.returncode}):\n{client_text}"
            )
        if "RESULT " in client_text:
            raise TestFailure(
                "conditional output was published after the deferred callback:\n"
                + client_text
            )

        client_process.stdin.write("2")
        client_process.stdin.flush()
        try:
            client_result = client_process.wait(timeout=5)
        except subprocess.TimeoutExpired as error:
            raise TestFailure("endpoint client timed out after completion") from error
        client_text = read_log(logs["client"])
        if client_result != 0 or "RESULT produced=2 received=1 value=3 seq=2" not in client_text:
            raise TestFailure(
                f"endpoint client failed ({client_result}):\n{client_text}"
            )

        filter_process.send_signal(signal.SIGTERM)
        try:
            filter_result = filter_process.wait(timeout=5)
        except subprocess.TimeoutExpired as error:
            raise TestFailure("ndarray filter did not stop cleanly") from error
        filter_text = read_log(logs["filter"])
        if filter_result != 0 or "RESULT callbacks=2 retained=1" not in filter_text:
            raise TestFailure(
                f"ndarray filter failed ({filter_result}):\n{filter_text}"
            )
        if daemon.poll() is not None:
            raise TestFailure("daemon exited during ndarray retention proof")
        print("connected ndarray conditional-output retention passed")
        return 0
    except TestFailure as error:
        print(f"connected ndarray retention test failed: {error}", file=sys.stderr)
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
