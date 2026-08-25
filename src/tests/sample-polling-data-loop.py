#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Collect cycle call graphs for the warmed polling data-loop scan."""

import argparse
import datetime
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


SUPPORT_PATH = Path(__file__).with_name("profile-polling-data-loop.py")
SUPPORT_SPEC = importlib.util.spec_from_file_location("polling_profile_support", SUPPORT_PATH)
SUPPORT = importlib.util.module_from_spec(SUPPORT_SPEC)
SUPPORT_SPEC.loader.exec_module(SUPPORT)


def run_sample(binary, environment, work_sources, samples, warmup,
               cpus, priority, histogram, perf_data):
    ready_r, ready_w = os.pipe()
    start_r, start_w = os.pipe()
    done_r, done_w = os.pipe()
    finish_r, finish_w = os.pipe()
    child_environment = environment.copy()
    child_environment.update(
        {
            "PW_BENCHMARK_PROFILE_READY_FD": str(ready_w),
            "PW_BENCHMARK_PROFILE_START_FD": str(start_r),
            "PW_BENCHMARK_PROFILE_DONE_FD": str(done_w),
            "PW_BENCHMARK_PROFILE_FINISH_FD": str(finish_r),
        }
    )
    command = [
        str(binary),
        "--benchmark-scan",
        str(work_sources),
        "none",
        str(samples),
        str(warmup),
        str(cpus[0]),
        str(cpus[1]),
        str(priority),
        str(histogram),
    ]
    child = subprocess.Popen(
        command,
        env=child_environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        pass_fds=(ready_w, start_r, done_w, finish_r),
    )
    for fd in (ready_w, start_r, done_w, finish_r):
        os.close(fd)
    perf = None
    try:
        tid = struct.unpack("i", SUPPORT.wait_read(ready_r, struct.calcsize("i")))[0]
        control_r, control_w = os.pipe()
        ack_r, ack_w = os.pipe()
        perf_command = [
            "perf",
            "record",
            "--output",
            str(perf_data),
            "--delay=-1",
            f"--control=fd:{control_r},{ack_w}",
            "--event",
            "cycles:u",
            "--freq",
            "4999",
            "--call-graph",
            "dwarf,8192",
            "--tid",
            str(tid),
        ]
        perf = subprocess.Popen(
            perf_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            pass_fds=(control_r, ack_w),
        )
        os.close(control_r)
        os.close(ack_w)
        os.write(control_w, b"enable\n")
        if not SUPPORT.valid_perf_ack(SUPPORT.wait_line(ack_r)):
            raise RuntimeError("perf did not acknowledge enable")
        os.write(start_w, b"\x01")
        SUPPORT.wait_read(done_r, 1, timeout=120)
        os.write(control_w, b"disable\n")
        if not SUPPORT.valid_perf_ack(SUPPORT.wait_line(ack_r)):
            raise RuntimeError("perf did not acknowledge disable")
        os.write(finish_w, b"\x01")
        child_stdout, child_stderr = child.communicate(timeout=30)
        perf_stdout, perf_stderr = perf.communicate(timeout=30)
        if child.returncode != 0:
            raise RuntimeError(f"benchmark failed: {child_stderr}")
        if perf.returncode != 0:
            raise RuntimeError(f"perf record failed: {perf_stderr}")
    finally:
        for fd in (ready_r, start_w, done_r, finish_w):
            try:
                os.close(fd)
            except OSError:
                pass
        for name in ("control_w", "ack_r"):
            fd = locals().get(name)
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        if child.poll() is None:
            child.kill()
            child.wait()
        if perf is not None and perf.poll() is None:
            perf.kill()
            perf.wait()
    values = SUPPORT.parse_benchmark_output(child_stdout)
    if int(values["observed-loop-cpu"]) != cpus[1]:
        raise RuntimeError(f"loop CPU mismatch: {values}")
    if priority > 0 and (
        int(values["scheduler-error"]) != 0
        or int(values["observed-policy"]) != os.SCHED_FIFO
        or int(values["observed-priority"]) != priority
    ):
        raise RuntimeError(f"real-time scheduler mismatch: {values}")
    return command, perf_command, child_stdout, child_stderr, perf_stdout, perf_stderr


def perf_report(perf_data, children):
    command = [
        "perf",
        "report",
        "--input",
        str(perf_data),
        "--stdio",
        "--percent-limit",
        "0.1",
        "--sort",
        "comm,dso,symbol",
    ]
    command.append("--children" if children else "--no-children")
    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=120,
    )
    if result.returncode != 0:
        raise RuntimeError(f"perf report failed: {result.stdout}")
    return command, result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--samples", type=int, default=5_000_000)
    parser.add_argument("--warmup", type=int, default=1_000_000)
    parser.add_argument("--caller-cpu", type=int)
    parser.add_argument("--loop-cpu", type=int)
    parser.add_argument("--rt-priority", type=int, default=88)
    parser.add_argument("--work-sources", type=int, action="append", default=[])
    args = parser.parse_args()

    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise SystemExit(f"refusing to overwrite output path: {output}")
    output.mkdir(parents=True, exist_ok=True)
    source_root = Path(__file__).resolve().parents[2]
    build_root = binary.parents[2]
    cpus, allowed = SUPPORT.select_cpus(args.caller_cpu, args.loop_cpu)
    work_sources = args.work_sources or [0, 64]
    environment = os.environ.copy()
    environment.update(
        {
            "PIPEWIREAO_SPA_PLUGIN_DIR": str(build_root / "spa/plugins"),
            "PIPEWIREAO_CONFIG_DIR": str(build_root / "src/daemon"),
            "PIPEWIREAO_MODULE_DIR": str(build_root / "src/modules"),
            "LD_LIBRARY_PATH": str(build_root / "src/pipewire"),
        }
    )
    SUPPORT.capture_environment(output, source_root, build_root, binary, cpus, allowed)
    with (output / "environment.txt").open("a", encoding="utf-8") as stream:
        stream.write(
            f"\n## sampler\n{Path(__file__).resolve()}\n"
            f"sha256={SUPPORT.sha256_file(Path(__file__).resolve())}\n"
        )
    SUPPORT.snapshot_system(output / "system-before.txt")
    started = datetime.datetime.now(datetime.timezone.utc)
    commands = []
    with tempfile.TemporaryDirectory(prefix="pw-polling-sample-") as temporary:
        temporary = Path(temporary)
        for sources in work_sources:
            print(f"sampling sources={sources}", flush=True)
            stem = f"cycles-sources{sources}-rt{args.rt_priority}"
            histogram = temporary / f"{stem}.csv"
            perf_data = temporary / f"{stem}.data"
            values = run_sample(
                binary,
                environment,
                sources,
                args.samples,
                args.warmup,
                cpus,
                args.rt_priority,
                histogram,
                perf_data,
            )
            SUPPORT.validate_histogram(histogram, args.samples)
            command, perf_command, child_out, child_err, perf_out, perf_err = values
            report_command, report = perf_report(perf_data, children=False)
            children_command, children_report = perf_report(perf_data, children=True)
            (output / f"{stem}-report.txt").write_text(report, encoding="utf-8")
            (output / f"{stem}-children-report.txt").write_text(
                children_report, encoding="utf-8"
            )
            buildids = SUPPORT.run_text(["perf", "buildid-list", "-i", str(perf_data)])
            (output / f"{stem}-buildids.txt").write_text(buildids, encoding="utf-8")
            commands.append(
                f"$ {' '.join(command)}\n$ {' '.join(perf_command)}\n"
                f"$ {' '.join(report_command)}\n$ {' '.join(children_command)}\n"
                f"benchmark-stdout:\n{child_out}benchmark-stderr:\n{child_err}"
                f"perf-stdout:\n{perf_out}perf-stderr:\n{perf_err}\n"
            )
    ended = datetime.datetime.now(datetime.timezone.utc)
    (output / "commands.log").write_text("\n".join(commands), encoding="utf-8")
    SUPPORT.snapshot_system(output / "system-after.txt")
    metadata = {
        "started_utc": started.isoformat(),
        "ended_utc": ended.isoformat(),
        "duration_seconds": (ended - started).total_seconds(),
        "samples": args.samples,
        "warmup": args.warmup,
        "caller_cpu": cpus[0],
        "loop_cpu": cpus[1],
        "rt_priority": args.rt_priority,
        "work_sources": work_sources,
        "event": "cycles:u",
        "frequency_hz": 4999,
        "call_graph": "dwarf,8192",
        "raw_perf_data_retained": False,
        "measurement_scope": "profiled data-loop TID between warmed scan gates",
    }
    (output / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    SUPPORT.write_manifest(output)


if __name__ == "__main__":
    main()
