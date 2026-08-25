#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Collect warmed, thread-scoped PMU counters for polling data-loop scans."""

import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import random
import select
import struct
import subprocess
import tempfile
import time


EVENT_GROUPS = {
    "core": "{cycles:u,instructions:u,branches:u,branch-misses:u}",
    "l1d": "{cycles:u,instructions:u,L1-dcache-loads:u,L1-dcache-load-misses:u}",
    "l2": (
        "{cycles:u,instructions:u,l2_request_g1.all_no_prefetch:u,"
        "l2_cache_req_stat.ic_dc_miss_in_l2:u}"
    ),
    "dtlb": "{cycles:u,instructions:u,l1_dtlb_misses:u,l2_dtlb_misses:u}",
    "faults": "{page-faults,minor-faults,major-faults,context-switches,cpu-migrations}",
}

SUMMARY_FIELDS = (
    "repetition",
    "group",
    "work_sources",
    "samples",
    "warmup",
    "caller_cpu",
    "loop_cpu",
    "requested_rt_priority",
    "observed_loop_cpu",
    "observed_policy",
    "observed_priority",
    "scheduler_error",
    "measurement_seconds",
    "rate_per_second",
    "p50_us",
    "p99_us",
    "p99_9_us",
    "max_us",
    "cycles_per_scan",
    "instructions_per_scan",
    "ipc",
    "branches_per_scan",
    "branch_miss_rate",
    "l1d_loads_per_scan",
    "l1d_misses_per_scan",
    "l1d_miss_rate",
    "l2_requests_per_scan",
    "l2_misses_per_scan",
    "l2_miss_rate",
    "l1_dtlb_misses_per_scan",
    "l2_dtlb_misses_per_scan",
    "l2_dtlb_miss_rate",
    "page_faults",
    "minor_faults",
    "major_faults",
    "context_switches",
    "cpu_migrations",
    "minimum_counter_coverage_percent",
    "raw_perf_stat",
)


def read_text(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace").strip()
    except OSError as error:
        return f"unavailable: {error}"


def run_text(command, cwd=None):
    try:
        result = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"unavailable: {error}"
    return f"exit={result.returncode}\n{result.stdout.strip()}"


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cpu_topology(cpu):
    root = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
    return read_text(root / "physical_package_id"), read_text(root / "core_id")


def select_cpus(caller_cpu, loop_cpu):
    allowed = sorted(os.sched_getaffinity(0))
    if caller_cpu is not None or loop_cpu is not None:
        if caller_cpu is None or loop_cpu is None:
            raise ValueError("--caller-cpu and --loop-cpu must be used together")
        selected = caller_cpu, loop_cpu
    else:
        physical = []
        seen = set()
        for cpu in allowed:
            topology = cpu_topology(cpu)
            if topology not in seen:
                physical.append(cpu)
                seen.add(topology)
        packages = {}
        for cpu in physical:
            packages.setdefault(cpu_topology(cpu)[0], []).append(cpu)
        candidates = max(packages.values(), key=len)
        if len(candidates) < 2:
            raise ValueError("two allowed physical CPU cores are required")
        selected = candidates[-2], candidates[-1]
    if selected[0] not in allowed or selected[1] not in allowed:
        raise ValueError(f"selected CPUs {selected} are not both in {allowed}")
    if cpu_topology(selected[0]) == cpu_topology(selected[1]):
        raise ValueError("caller and loop CPUs must be different physical cores")
    return selected, allowed


def snapshot_system(path):
    sections = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "loadavg": read_text("/proc/loadavg"),
        "uptime": read_text("/proc/uptime"),
        "stat": read_text("/proc/stat"),
        "interrupts": read_text("/proc/interrupts"),
        "softirqs": read_text("/proc/softirqs"),
    }
    with path.open("w", encoding="utf-8") as stream:
        for name, value in sections.items():
            stream.write(f"## {name}\n{value}\n\n")


def capture_environment(output, source_root, build_root, binary, cpus, allowed):
    runner = Path(__file__).resolve()
    entries = {
        "uname": run_text(["uname", "-a"]),
        "lscpu": run_text(["lscpu"]),
        "perf-version": run_text(["perf", "--version"]),
        "perf-event-paranoid": read_text("/proc/sys/kernel/perf_event_paranoid"),
        "meson-configuration": run_text(["meson", "configure", str(build_root)]),
        "git-head": run_text(["git", "rev-parse", "HEAD"], cwd=source_root),
        "git-status": run_text(["git", "status", "--short"], cwd=source_root),
        "kernel-command-line": read_text("/proc/cmdline"),
        "process-limits": read_text("/proc/self/limits"),
        "cpu-isolated": read_text("/sys/devices/system/cpu/isolated"),
        "nohz-full": read_text("/sys/devices/system/cpu/nohz_full"),
        "selected-cpus": (
            f"caller={cpus[0]} topology={cpu_topology(cpus[0])}\n"
            f"loop={cpus[1]} topology={cpu_topology(cpus[1])}\n"
            f"allowed={allowed}"
        ),
        "binary": f"{binary}\nsha256={sha256_file(binary)}",
        "profiler-runner": f"{runner}\nsha256={sha256_file(runner)}",
    }
    for role, cpu in zip(("caller", "loop"), cpus):
        root = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
        for name in (
            "scaling_driver",
            "scaling_governor",
            "energy_performance_preference",
            "scaling_min_freq",
            "scaling_max_freq",
        ):
            entries[f"{role}-cpu-{cpu}-{name}"] = read_text(root / name)
    with (output / "environment.txt").open("w", encoding="utf-8") as stream:
        stream.write(
            f"captured-utc={datetime.datetime.now(datetime.timezone.utc).isoformat()}\n"
        )
        for name, value in entries.items():
            stream.write(f"\n## {name}\n{value}\n")
    (output / "perf-list.txt").write_text(
        run_text(["perf", "list", "--details"]), encoding="utf-8"
    )
    source_diff = subprocess.run(
        ["git", "diff", "--binary", "HEAD"],
        cwd=source_root,
        stdout=subprocess.PIPE,
        check=True,
    ).stdout
    (output / "source.patch").write_bytes(source_diff)


def wait_read(fd, size, timeout=30):
    result = bytearray()
    deadline = time.monotonic() + timeout
    while len(result) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise TimeoutError(f"timed out reading fd {fd}")
        chunk = os.read(fd, size - len(result))
        if not chunk:
            raise EOFError(f"unexpected EOF on fd {fd}")
        result.extend(chunk)
    return bytes(result)


def wait_line(fd, timeout=30):
    result = bytearray()
    while not result.endswith(b"\n"):
        result.extend(wait_read(fd, 1, timeout))
    return bytes(result)


def valid_perf_ack(value):
    return value.lstrip(b"\0") == b"ack\n"


def parse_benchmark_output(output):
    lines = [line for line in output.splitlines() if line.startswith("scan ")]
    if len(lines) != 1:
        raise ValueError(f"unexpected benchmark output: {output!r}")
    values = {}
    for token in lines[0].split()[1:]:
        key, value = token.split("=", 1)
        values[key] = value
    return values


def validate_histogram(path, samples):
    total = 0
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            total += int(row["count"])
    if total != samples:
        raise ValueError(f"{path} contains {total} samples, expected {samples}")


def parse_perf_stat(path):
    counters = {}
    coverage = []
    with path.open(encoding="utf-8") as stream:
        for fields in csv.reader(stream, delimiter=";"):
            if not fields or fields[0].startswith("#"):
                continue
            if fields[0].startswith("<not"):
                raise ValueError(f"counter unavailable in {path}: {fields}")
            if len(fields) < 5:
                raise ValueError(f"invalid perf stat row in {path}: {fields}")
            event = fields[2].removesuffix(":u")
            counters[event] = int(float(fields[0]))
            coverage.append(float(fields[4]))
    if not counters:
        raise ValueError(f"no counters in {path}")
    minimum = min(coverage)
    if minimum < 99.9:
        raise ValueError(f"multiplexed counter group in {path}: {minimum}%")
    return counters, minimum


def ratio(numerator, denominator):
    return float(numerator) / float(denominator) if denominator else 0.0


def add_derived_counters(row, counters, samples):
    for field in SUMMARY_FIELDS:
        row.setdefault(field, "")
    cycles = counters.get("cycles")
    instructions = counters.get("instructions")
    if cycles is not None:
        row["cycles_per_scan"] = cycles / samples
    if instructions is not None:
        row["instructions_per_scan"] = instructions / samples
    if cycles is not None and instructions is not None:
        row["ipc"] = ratio(instructions, cycles)
    mappings = {
        "branches": "branches_per_scan",
        "L1-dcache-loads": "l1d_loads_per_scan",
        "L1-dcache-load-misses": "l1d_misses_per_scan",
        "l2_request_g1.all_no_prefetch": "l2_requests_per_scan",
        "l2_cache_req_stat.ic_dc_miss_in_l2": "l2_misses_per_scan",
        "l1_dtlb_misses": "l1_dtlb_misses_per_scan",
        "l2_dtlb_misses": "l2_dtlb_misses_per_scan",
    }
    for event, field in mappings.items():
        if event in counters:
            row[field] = counters[event] / samples
    if "branches" in counters and "branch-misses" in counters:
        row["branch_miss_rate"] = ratio(counters["branch-misses"], counters["branches"])
    if "L1-dcache-loads" in counters and "L1-dcache-load-misses" in counters:
        row["l1d_miss_rate"] = ratio(
            counters["L1-dcache-load-misses"], counters["L1-dcache-loads"]
        )
    if (
        "l2_request_g1.all_no_prefetch" in counters
        and "l2_cache_req_stat.ic_dc_miss_in_l2" in counters
    ):
        row["l2_miss_rate"] = ratio(
            counters["l2_cache_req_stat.ic_dc_miss_in_l2"],
            counters["l2_request_g1.all_no_prefetch"],
        )
    if "l1_dtlb_misses" in counters and "l2_dtlb_misses" in counters:
        row["l2_dtlb_miss_rate"] = ratio(
            counters["l2_dtlb_misses"], counters["l1_dtlb_misses"]
        )
    for event, field in (
        ("page-faults", "page_faults"),
        ("minor-faults", "minor_faults"),
        ("major-faults", "major_faults"),
        ("context-switches", "context_switches"),
        ("cpu-migrations", "cpu_migrations"),
    ):
        if event in counters:
            row[field] = counters[event]


def run_profile(binary, output, environment, group, work_sources, samples, warmup,
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
    raw = output / f"stat-{group}-sources{work_sources}-rt{priority}-rep{repetition}.csv"
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
    perf_command = None
    try:
        tid = struct.unpack("i", wait_read(ready_r, struct.calcsize("i")))[0]
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
            EVENT_GROUPS[group],
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
        if not valid_perf_ack(wait_line(ack_r)):
            raise RuntimeError("perf did not acknowledge enable")
        measured_started = time.monotonic()
        os.write(start_w, b"\x01")
        wait_read(done_r, 1, timeout=120)
        measurement_seconds = time.monotonic() - measured_started
        os.write(control_w, b"disable\n")
        disable_ack = wait_line(ack_r)
        if not valid_perf_ack(disable_ack):
            raise RuntimeError(f"perf did not acknowledge disable: {disable_ack!r}")
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
    counters, coverage = parse_perf_stat(raw)
    values = parse_benchmark_output(child_stdout)
    row = {
        "repetition": repetition,
        "group": group,
        "work_sources": work_sources,
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
    add_derived_counters(row, counters, samples)
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
    command_log = (
        f"$ {' '.join(command)}\n"
        f"$ {' '.join(perf_command)}\n"
        f"benchmark-stdout:\n{child_stdout}benchmark-stderr:\n{child_stderr}"
        f"perf-stdout:\n{perf_stdout}perf-stderr:\n{perf_stderr}\n"
    )
    return row, command_log


def write_aggregates(rows, output):
    keys = ("group", "work_sources", "requested_rt_priority")
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


def write_manifest(output):
    with (output / "SHA256SUMS").open("w", encoding="utf-8") as stream:
        for path in sorted(output.iterdir()):
            if path.name != "SHA256SUMS" and path.is_file():
                stream.write(f"{sha256_file(path)}  {path.name}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--samples", type=int, default=1_000_000)
    parser.add_argument("--warmup", type=int, default=1_000_000)
    parser.add_argument("--caller-cpu", type=int)
    parser.add_argument("--loop-cpu", type=int)
    parser.add_argument("--rt-priority", type=int, action="append", default=[])
    parser.add_argument("--work-sources", type=int, action="append", default=[])
    parser.add_argument("--group", choices=EVENT_GROUPS, action="append", default=[])
    parser.add_argument("--seed", type=int, default=20260825)
    args = parser.parse_args()

    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise SystemExit(f"refusing to overwrite output path: {output}")
    output.mkdir(parents=True, exist_ok=True)
    source_root = Path(__file__).resolve().parents[2]
    build_root = binary.parents[2]
    cpus, allowed = select_cpus(args.caller_cpu, args.loop_cpu)
    priorities = args.rt_priority or [0, 88]
    work_sources = args.work_sources or [0, 1, 4, 16, 64]
    event_groups = args.group or list(EVENT_GROUPS)
    environment = os.environ.copy()
    environment.update(
        {
            "PIPEWIREAO_SPA_PLUGIN_DIR": str(build_root / "spa/plugins"),
            "PIPEWIREAO_CONFIG_DIR": str(build_root / "src/daemon"),
            "PIPEWIREAO_MODULE_DIR": str(build_root / "src/modules"),
            "LD_LIBRARY_PATH": str(build_root / "src/pipewire"),
        }
    )
    capture_environment(output, source_root, build_root, binary, cpus, allowed)
    snapshot_system(output / "system-before.txt")
    cases = []
    for repetition in range(1, args.repetitions + 1):
        repeated = [
            (repetition, group, sources, priority)
            for group in event_groups
            for sources in work_sources
            for priority in priorities
        ]
        random.Random(args.seed + repetition).shuffle(repeated)
        cases.extend(repeated)

    rows = []
    logs = []
    started = datetime.datetime.now(datetime.timezone.utc)
    with tempfile.TemporaryDirectory(prefix="pw-polling-profile-") as temporary:
        temporary = Path(temporary)
        for index, (repetition, group, sources, priority) in enumerate(cases, 1):
            stem = f"{group}-sources{sources}-rt{priority}-rep{repetition}"
            print(f"[{index}/{len(cases)}] {stem}", flush=True)
            histogram = temporary / f"{stem}.csv"
            row, log = run_profile(
                binary,
                output,
                environment,
                group,
                sources,
                args.samples,
                args.warmup,
                cpus,
                priority,
                repetition,
                histogram,
            )
            validate_histogram(histogram, args.samples)
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
    snapshot_system(output / "system-after.txt")
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
        "work_sources": work_sources,
        "event_groups": {name: EVENT_GROUPS[name] for name in event_groups},
        "measurement_scope": "profiled data-loop TID between warmed scan gates",
    }
    (output / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    write_manifest(output)


if __name__ == "__main__":
    main()
