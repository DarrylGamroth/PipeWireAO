#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Collect warmed, data-loop-thread PMU counters for graph activations."""

import argparse
import csv
import datetime
import importlib.util
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import time


SUPPORT_PATH = Path(__file__).with_name("profile-polling-data-loop.py")
SUPPORT_SPEC = importlib.util.spec_from_file_location("polling_profile_support", SUPPORT_PATH)
SUPPORT = importlib.util.module_from_spec(SUPPORT_SPEC)
SUPPORT_SPEC.loader.exec_module(SUPPORT)

SUMMARY_FIELDS = tuple(
    field.replace("_per_scan", "_per_activation")
    for field in SUPPORT.SUMMARY_FIELDS
    if field != "work_sources"
)
SUMMARY_FIELDS = SUMMARY_FIELDS[:2] + ("idle",) + SUMMARY_FIELDS[2:]


def derive(row, counters, samples):
    temporary = {field: "" for field in SUPPORT.SUMMARY_FIELDS}
    SUPPORT.add_derived_counters(temporary, counters, samples)
    for field, value in temporary.items():
        target = field.replace("_per_scan", "_per_activation")
        if target in SUMMARY_FIELDS and value != "":
            row[target] = value


def run_profile(binary, output, environment, group, idle, samples, warmup,
                cpus, priority, repetition, histogram):
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
        "--benchmark-activation",
        idle,
        str(samples),
        str(warmup),
        str(cpus[0]),
        str(cpus[1]),
        str(priority),
        str(histogram),
    ]
    raw = output / f"stat-{group}-{idle}-rt{priority}-rep{repetition}.csv"
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
            "stat",
            "--no-big-num",
            "-x",
            ";",
            "--output",
            str(raw),
            "--delay=-1",
            f"--control=fd:{control_r},{ack_w}",
            "-e",
            SUPPORT.EVENT_GROUPS[group],
            "-t",
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
        measured_started = time.monotonic()
        os.write(start_w, b"\x01")
        SUPPORT.wait_read(done_r, 1, timeout=120)
        measurement_seconds = time.monotonic() - measured_started
        os.write(control_w, b"disable\n")
        if not SUPPORT.valid_perf_ack(SUPPORT.wait_line(ack_r)):
            raise RuntimeError("perf did not acknowledge disable")
        os.write(finish_w, b"\x01")
        child_stdout, child_stderr = child.communicate(timeout=30)
        perf_stdout, perf_stderr = perf.communicate(timeout=30)
        if child.returncode != 0:
            raise RuntimeError(f"benchmark failed: {child_stderr}")
        if perf.returncode != 0:
            raise RuntimeError(f"perf stat failed: {perf_stderr}")
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
    counters, coverage = SUPPORT.parse_perf_stat(raw)
    values = SUPPORT.parse_benchmark_output(child_stdout.replace("activation ", "scan ", 1))
    row = {field: "" for field in SUMMARY_FIELDS}
    row.update(
        {
            "repetition": repetition,
            "group": group,
            "idle": idle,
            "samples": samples,
            "warmup": warmup,
            "caller_cpu": cpus[0],
            "loop_cpu": cpus[1],
            "requested_rt_priority": priority,
            "observed_loop_cpu": values["observed-loop-cpu"],
            "observed_policy": values["observed-policy"],
            "observed_priority": values["observed-priority"],
            "scheduler_error": values["scheduler-error"],
            "measurement_seconds": measurement_seconds,
            "rate_per_second": values["rate"].removesuffix("/s"),
            "p50_us": values["p50"].removesuffix("us"),
            "p99_us": values["p99"].removesuffix("us"),
            "p99_9_us": values["p99.9"].removesuffix("us"),
            "max_us": values["max"].removesuffix("us"),
            "minimum_counter_coverage_percent": coverage,
            "raw_perf_stat": raw.name,
        }
    )
    derive(row, counters, samples)
    if int(row["observed_loop_cpu"]) != cpus[1]:
        raise RuntimeError(f"loop CPU mismatch: {row}")
    if priority > 0 and (
        int(row["scheduler_error"]) != 0
        or int(row["observed_policy"]) != os.SCHED_FIFO
        or int(row["observed_priority"]) != priority
    ):
        raise RuntimeError(f"real-time scheduler mismatch: {row}")
    if int(row.get("cpu_migrations", 0) or 0) != 0:
        raise RuntimeError(f"profiled thread migrated: {row}")
    log = (
        f"$ {' '.join(command)}\n$ {' '.join(perf_command)}\n"
        f"benchmark-stdout:\n{child_stdout}benchmark-stderr:\n{child_stderr}"
        f"perf-stdout:\n{perf_stdout}perf-stderr:\n{perf_stderr}\n"
    )
    return row, log


def write_aggregates(rows, output):
    keys = ("group", "idle", "requested_rt_priority")
    excluded = set(keys) | {
        "repetition",
        "samples",
        "warmup",
        "caller_cpu",
        "loop_cpu",
        "observed_loop_cpu",
        "observed_policy",
        "observed_priority",
        "scheduler_error",
        "raw_perf_stat",
    }
    metrics = [field for field in SUMMARY_FIELDS if field not in excluded]
    groups = {}
    for row in rows:
        key = tuple(str(row[name]) for name in keys)
        groups.setdefault(key, []).append(row)
    fields = list(keys) + ["repetitions"]
    for metric in metrics:
        fields.extend((f"{metric}_min", f"{metric}_median", f"{metric}_max"))
    with (output / "aggregate.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fields)
        writer.writeheader()
        for key, members in sorted(groups.items()):
            result = dict(zip(keys, key))
            result["repetitions"] = len(members)
            for metric in metrics:
                values = sorted(
                    float(member[metric]) for member in members if member.get(metric) != ""
                )
                if values:
                    result[f"{metric}_min"] = values[0]
                    result[f"{metric}_median"] = values[len(values) // 2]
                    result[f"{metric}_max"] = values[-1]
            writer.writerow(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--samples", type=int, default=100_000)
    parser.add_argument("--warmup", type=int, default=100_000)
    parser.add_argument("--caller-cpu", type=int)
    parser.add_argument("--loop-cpu", type=int)
    parser.add_argument("--rt-priority", type=int, action="append", default=[])
    parser.add_argument("--idle", choices=("busy-spin", "eventfd"), action="append", default=[])
    parser.add_argument("--group", choices=SUPPORT.EVENT_GROUPS, action="append", default=[])
    parser.add_argument("--seed", type=int, default=20260825)
    args = parser.parse_args()

    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise SystemExit(f"refusing to overwrite output path: {output}")
    output.mkdir(parents=True, exist_ok=True)
    source_root = Path(__file__).resolve().parents[2]
    build_root = binary.parents[2]
    cpus, allowed = SUPPORT.select_cpus(args.caller_cpu, args.loop_cpu)
    priorities = args.rt_priority or [0, 88]
    idle_modes = args.idle or ["busy-spin", "eventfd"]
    event_groups = args.group or list(SUPPORT.EVENT_GROUPS)
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
            f"\n## activation-profiler\n{Path(__file__).resolve()}\n"
            f"sha256={SUPPORT.sha256_file(Path(__file__).resolve())}\n"
        )
    SUPPORT.snapshot_system(output / "system-before.txt")
    cases = []
    for repetition in range(1, args.repetitions + 1):
        repeated = [
            (repetition, group, idle, priority)
            for group in event_groups
            for idle in idle_modes
            for priority in priorities
        ]
        random.Random(args.seed + repetition).shuffle(repeated)
        cases.extend(repeated)
    rows = []
    logs = []
    started = datetime.datetime.now(datetime.timezone.utc)
    with tempfile.TemporaryDirectory(prefix="pw-activation-profile-") as temporary:
        temporary = Path(temporary)
        for index, (repetition, group, idle, priority) in enumerate(cases, 1):
            stem = f"{group}-{idle}-rt{priority}-rep{repetition}"
            print(f"[{index}/{len(cases)}] {stem}", flush=True)
            histogram = temporary / f"{stem}.csv"
            row, log = run_profile(
                binary,
                output,
                environment,
                group,
                idle,
                args.samples,
                args.warmup,
                cpus,
                priority,
                repetition,
                histogram,
            )
            SUPPORT.validate_histogram(histogram, args.samples)
            rows.append(row)
            logs.append(log)
            (output / "commands.log").write_text("\n".join(logs), encoding="utf-8")
            time.sleep(0.05)
    ended = datetime.datetime.now(datetime.timezone.utc)
    with (output / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, SUMMARY_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    write_aggregates(rows, output)
    SUPPORT.snapshot_system(output / "system-after.txt")
    metadata = {
        "started_utc": started.isoformat(),
        "ended_utc": ended.isoformat(),
        "duration_seconds": (ended - started).total_seconds(),
        "random_seed": args.seed,
        "repetitions": args.repetitions,
        "samples": args.samples,
        "warmup": args.warmup,
        "caller_cpu": cpus[0],
        "loop_cpu": cpus[1],
        "rt_priorities": priorities,
        "idle_modes": idle_modes,
        "event_groups": {name: SUPPORT.EVENT_GROUPS[name] for name in event_groups},
        "measurement_scope": "activation consumer data-loop TID between warmed gates",
    }
    (output / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    SUPPORT.write_manifest(output)


if __name__ == "__main__":
    main()
