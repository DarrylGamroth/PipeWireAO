#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Run and record the polling data-loop latency experiment."""

import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import subprocess
import time


SUMMARY_FIELDS = (
    "benchmark",
    "repetition",
    "idle",
    "work_sources",
    "contention",
    "samples",
    "warmup",
    "model",
    "caller_cpu",
    "loop_cpu",
    "requested_rt_priority",
    "observed_loop_cpu",
    "observed_policy",
    "observed_priority",
    "scheduler_error",
    "rate_per_second",
    "p50_us",
    "p90_us",
    "p99_us",
    "p99_9_us",
    "max_us",
    "control_submitted",
    "control_completed",
    "control_failed",
    "histogram",
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


def cpu_topology(cpu):
    root = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
    return (
        read_text(root / "physical_package_id"),
        read_text(root / "core_id"),
    )


def select_cpus(caller_cpu, loop_cpu):
    allowed = sorted(os.sched_getaffinity(0))
    if caller_cpu is not None or loop_cpu is not None:
        if caller_cpu is None or loop_cpu is None:
            raise ValueError("--caller-cpu and --loop-cpu must be used together")
        selected = (caller_cpu, loop_cpu)
    else:
        physical = []
        seen = set()
        for cpu in allowed:
            topology = cpu_topology(cpu)
            if topology not in seen:
                physical.append(cpu)
                seen.add(topology)
        same_package = {}
        for cpu in physical:
            same_package.setdefault(cpu_topology(cpu)[0], []).append(cpu)
        candidates = max(same_package.values(), key=len)
        if len(candidates) < 2:
            raise ValueError("two allowed physical CPU cores are required")
        selected = (candidates[-2], candidates[-1])
    if selected[0] not in allowed or selected[1] not in allowed:
        raise ValueError(f"selected CPUs {selected} are not both in {allowed}")
    if cpu_topology(selected[0]) == cpu_topology(selected[1]):
        raise ValueError("caller and loop CPUs must be different physical cores")
    return selected, allowed


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


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
    files = {
        "uname": run_text(["uname", "-a"]),
        "lscpu-summary": run_text(["lscpu"]),
        "lscpu": run_text(["lscpu", "-e=CPU,NODE,SOCKET,CORE,ONLINE,MAXMHZ,MINMHZ"]),
        "numactl": run_text(["numactl", "--hardware"]),
        "compiler": run_text(["cc", "--version"]),
        "meson": run_text(["meson", "--version"]),
        "ninja": run_text(["ninja", "--version"]),
        "meson-configuration": run_text(["meson", "configure", str(build_root)]),
        "git-head": run_text(["git", "rev-parse", "HEAD"], cwd=source_root),
        "git-status": run_text(["git", "status", "--short"], cwd=source_root),
        "git-diff-stat": run_text(["git", "diff", "--stat", "HEAD"], cwd=source_root),
        "kernel-command-line": read_text("/proc/cmdline"),
        "process-limits": read_text("/proc/self/limits"),
        "process-status": read_text("/proc/self/status"),
        "cpu-online": read_text("/sys/devices/system/cpu/online"),
        "cpu-isolated": read_text("/sys/devices/system/cpu/isolated"),
        "nohz-full": read_text("/sys/devices/system/cpu/nohz_full"),
        "kernel-sched-rt-runtime-us": read_text("/proc/sys/kernel/sched_rt_runtime_us"),
        "kernel-sched-rt-period-us": read_text("/proc/sys/kernel/sched_rt_period_us"),
        "turbo-intel-no-turbo": read_text("/sys/devices/system/cpu/intel_pstate/no_turbo"),
        "turbo-cpufreq-boost": read_text("/sys/devices/system/cpu/cpufreq/boost"),
        "rt-capability": run_text(["chrt", "--fifo", "88", "true"]),
    }
    for role, cpu in zip(("caller", "loop"), cpus):
        root = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
        for name in (
            "scaling_driver",
            "scaling_governor",
            "scaling_min_freq",
            "scaling_max_freq",
        ):
            files[f"{role}-cpu-{cpu}-{name}"] = read_text(root / name)
    files["selected-cpus"] = (
        f"caller={cpus[0]} topology={cpu_topology(cpus[0])}\n"
        f"loop={cpus[1]} topology={cpu_topology(cpus[1])}\n"
        f"allowed={allowed}"
    )
    files["binary"] = f"{binary}\nsha256={sha256_file(binary)}"
    files["benchmark-runner"] = f"{runner}\nsha256={sha256_file(runner)}"
    with (output / "environment.txt").open("w", encoding="utf-8") as stream:
        stream.write(f"captured-utc={datetime.datetime.now(datetime.timezone.utc).isoformat()}\n")
        stream.write(f"python={platform.python_version()}\n")
        for name, value in files.items():
            stream.write(f"\n## {name}\n{value}\n")
    diff = subprocess.run(
        ["git", "diff", "--binary", "HEAD"],
        cwd=source_root,
        stdout=subprocess.PIPE,
        check=True,
    ).stdout
    (output / "source.patch").write_bytes(diff)


def parse_summary(line, repetition, histogram):
    tokens = line.strip().split()
    if not tokens or tokens[0] not in ("activation", "scan"):
        raise ValueError(f"unexpected benchmark output: {line!r}")
    values = {}
    for token in tokens[1:]:
        key, value = token.split("=", 1)
        values[key] = value
    row = {field: "" for field in SUMMARY_FIELDS}
    row["benchmark"] = tokens[0]
    row["repetition"] = repetition
    mappings = {
        "idle": "idle",
        "work-sources": "work_sources",
        "contention": "contention",
        "samples": "samples",
        "warmup": "warmup",
        "model": "model",
        "caller-cpu": "caller_cpu",
        "loop-cpu": "loop_cpu",
        "requested-rt-priority": "requested_rt_priority",
        "observed-loop-cpu": "observed_loop_cpu",
        "observed-policy": "observed_policy",
        "observed-priority": "observed_priority",
        "scheduler-error": "scheduler_error",
        "control-submitted": "control_submitted",
        "control-completed": "control_completed",
        "control-failed": "control_failed",
    }
    for source, destination in mappings.items():
        if source in values:
            row[destination] = values[source]
    row["rate_per_second"] = values["rate"].removesuffix("/s")
    for source, destination in (
        ("p50", "p50_us"),
        ("p90", "p90_us"),
        ("p99", "p99_us"),
        ("p99.9", "p99_9_us"),
        ("max", "max_us"),
    ):
        row[destination] = values[source].removesuffix("us")
    row["histogram"] = histogram.name
    return row


def validate_histogram(path, samples):
    total = 0
    previous = -1
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["latency_ns", "count"]:
            raise ValueError(f"invalid histogram header in {path}")
        for row in reader:
            latency = int(row["latency_ns"])
            if latency <= previous:
                raise ValueError(f"unsorted histogram in {path}")
            previous = latency
            total += int(row["count"])
    if total != samples:
        raise ValueError(f"{path} contains {total} samples, expected {samples}")


def write_aggregates(rows, output):
    keys = ("benchmark", "idle", "work_sources", "contention", "requested_rt_priority")
    metrics = ("rate_per_second", "p50_us", "p90_us", "p99_us", "p99_9_us", "max_us")
    groups = {}
    for row in rows:
        key = tuple(row[name] for name in keys)
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
                values = sorted(float(member[metric]) for member in members)
                result[f"{metric}_min"] = values[0]
                result[f"{metric}_median"] = values[len(values) // 2]
                result[f"{metric}_max"] = values[-1]
            writer.writerow(result)


def write_manifest(output):
    with (output / "SHA256SUMS").open("w", encoding="utf-8") as stream:
        for path in sorted(output.iterdir()):
            if path.name == "SHA256SUMS" or not path.is_file():
                continue
            stream.write(f"{sha256_file(path)}  {path.name}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--activation-samples", type=int, default=100_000)
    parser.add_argument("--activation-warmup", type=int, default=10_000)
    parser.add_argument("--scan-samples", type=int, default=1_000_000)
    parser.add_argument("--scan-warmup", type=int, default=100_000)
    parser.add_argument("--caller-cpu", type=int)
    parser.add_argument("--loop-cpu", type=int)
    parser.add_argument("--rt-priority", type=int, action="append", default=[])
    parser.add_argument("--seed", type=int, default=20260824)
    args = parser.parse_args()

    binary = args.binary.resolve(strict=True)
    build_root = binary.parents[2]
    source_root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if output.exists():
        if not output.is_dir() or any(output.iterdir()):
            raise SystemExit(f"refusing to overwrite output path: {output}")
    output.mkdir(parents=True, exist_ok=True)
    cpus, allowed = select_cpus(args.caller_cpu, args.loop_cpu)
    priorities = args.rt_priority or [0]
    if any(priority < 0 or priority > 99 for priority in priorities):
        raise SystemExit("RT priorities must be between 0 and 99")

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
        repeated = []
        for priority in priorities:
            for idle in ("eventfd", "busy-spin"):
                repeated.append(("activation", repetition, priority, idle, None, None))
            for work_sources in (0, 1, 4, 16, 64):
                for contention in ("none", "periodic", "burst"):
                    repeated.append(
                        ("scan", repetition, priority, None, work_sources, contention)
                    )
        random.Random(args.seed + repetition).shuffle(repeated)
        cases.extend(repeated)

    rows = []
    commands = []
    started = datetime.datetime.now(datetime.timezone.utc)
    for index, (kind, repetition, priority, idle, work_sources, contention) in enumerate(cases, 1):
        if kind == "activation":
            stem = f"activation-{idle}-rt{priority}-rep{repetition}"
            command = [
                str(binary),
                "--benchmark-activation",
                idle,
                str(args.activation_samples),
                str(args.activation_warmup),
                str(cpus[0]),
                str(cpus[1]),
                str(priority),
            ]
            samples = args.activation_samples
        else:
            stem = f"scan-{work_sources}-{contention}-rt{priority}-rep{repetition}"
            command = [
                str(binary),
                "--benchmark-scan",
                str(work_sources),
                contention,
                str(args.scan_samples),
                str(args.scan_warmup),
                str(cpus[0]),
                str(cpus[1]),
                str(priority),
            ]
            samples = args.scan_samples
        histogram = output / f"{stem}.csv"
        command.append(str(histogram))
        print(f"[{index}/{len(cases)}] {stem}", flush=True)
        run_started = time.monotonic()
        result = subprocess.run(
            command,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=120,
        )
        duration = time.monotonic() - run_started
        commands.append(
            f"$ {' '.join(command)}\nexit={result.returncode} duration={duration:.6f}s\n"
            f"stdout:\n{result.stdout}stderr:\n{result.stderr}\n"
        )
        (output / "commands.log").write_text("\n".join(commands), encoding="utf-8")
        if result.returncode != 0:
            raise SystemExit(f"benchmark failed: {stem}; see commands.log")
        validate_histogram(histogram, samples)
        row = parse_summary(result.stdout.strip(), repetition, histogram)
        if int(row["observed_loop_cpu"]) != cpus[1]:
            raise SystemExit(f"loop CPU placement failed for {stem}: {row}")
        if priority > 0 and (
            int(row["scheduler_error"]) != 0
            or int(row["observed_policy"]) != os.SCHED_FIFO
            or int(row["observed_priority"]) != priority
        ):
            raise SystemExit(f"real-time scheduling failed for {stem}: {row}")
        rows.append(row)
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
        "activation_samples": args.activation_samples,
        "activation_warmup": args.activation_warmup,
        "scan_samples": args.scan_samples,
        "scan_warmup": args.scan_warmup,
        "caller_cpu": cpus[0],
        "loop_cpu": cpus[1],
        "rt_priorities": priorities,
        "histogram_format": "exact run-length counts of integer nanoseconds",
    }
    (output / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    write_manifest(output)


if __name__ == "__main__":
    main()
