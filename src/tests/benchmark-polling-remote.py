#!/usr/bin/env python3
# PipeWire
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Benchmark buffer transit through exported nodes and an isolated daemon."""

import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import random
import select
import statistics
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

DEFAULT_MODES = (
    "eventfd-eventfd",
    "eventfd-busy-spin",
    "busy-spin-eventfd",
    "busy-spin-busy-spin",
)

LATENCY_COLUMNS = (
    "trigger_to_source_ns",
    "source_to_sink_ns",
    "trigger_to_sink_ns",
)


class BenchmarkFailure(RuntimeError):
    pass


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


def select_cpus(source_cpu, sink_cpu):
    allowed = sorted(os.sched_getaffinity(0))
    if source_cpu is not None or sink_cpu is not None:
        if source_cpu is None or sink_cpu is None:
            raise ValueError("--source-cpu and --sink-cpu must be used together")
        selected = source_cpu, sink_cpu
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
        raise ValueError("source and sink must use different physical cores")
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


def capture_environment(output, source_root, build_root, paths, cpus, allowed):
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
            f"source={cpus[0]} topology={cpu_topology(cpus[0])}\n"
            f"sink={cpus[1]} topology={cpu_topology(cpus[1])}\n"
            f"allowed={allowed}"
        ),
        "benchmark-runner": f"{runner}\nsha256={sha256_file(runner)}",
    }
    for name, path in paths.items():
        entries[name] = f"{path}\nsha256={sha256_file(path)}"
    for role, cpu in zip(("source", "sink"), cpus):
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


def wait_for(predicate, timeout, description):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)
    raise BenchmarkFailure(f"timed out waiting for {description}")


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


def link_streams(paths, environment, client, name):
    def try_link():
        if client.poll() is not None:
            raise BenchmarkFailure(f"{name} exited before its streams linked")
        try:
            result = subprocess.run(
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
        return result.returncode == 0

    wait_for(try_link, 5, f"{name} exported stream link")


def parse_result(output):
    lines = [line for line in output.splitlines() if line.startswith("BENCHMARK ")]
    if len(lines) != 1:
        raise BenchmarkFailure(f"unexpected benchmark output: {output!r}")
    values = {}
    for token in lines[0].split()[1:]:
        key, value = token.split("=", 1)
        values[key] = int(value)
    return values


def nearest_rank(values, numerator, denominator):
    rank = (len(values) * numerator + denominator - 1) // denominator
    return values[max(1, rank) - 1]


def summarize_latencies(path, samples):
    columns = {name: [] for name in LATENCY_COLUMNS}
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            for name in LATENCY_COLUMNS:
                columns[name].append(int(row[name]))
    if any(len(values) != samples for values in columns.values()):
        raise BenchmarkFailure(f"{path} does not contain {samples} complete samples")
    result = {}
    for name, values in columns.items():
        ordered = sorted(values)
        result[f"{name}_mean"] = statistics.fmean(values)
        result[f"{name}_p50"] = nearest_rank(ordered, 50, 100)
        result[f"{name}_p90"] = nearest_rank(ordered, 90, 100)
        result[f"{name}_p99"] = nearest_rank(ordered, 99, 100)
        result[f"{name}_p99_9"] = nearest_rank(ordered, 999, 1000)
        result[f"{name}_max"] = ordered[-1]
    return result


def parse_perf_stat(path):
    counters = {}
    coverage = []
    with path.open(encoding="utf-8") as stream:
        for fields in csv.reader(stream, delimiter=";"):
            if not fields or fields[0].startswith("#"):
                continue
            if fields[0].startswith("<not") or len(fields) < 5:
                raise BenchmarkFailure(f"invalid counter row in {path}: {fields}")
            counters[fields[2].removesuffix(":u")] = int(float(fields[0]))
            coverage.append(float(fields[4]))
    if not counters or min(coverage) < 99.9:
        raise BenchmarkFailure(f"missing or multiplexed counters in {path}")
    return counters, min(coverage)


def add_counters(row, counters, samples):
    cycles = counters.get("cycles")
    instructions = counters.get("instructions")
    if cycles is not None:
        row["cycles_per_frame"] = cycles / samples
    if instructions is not None:
        row["instructions_per_frame"] = instructions / samples
    if cycles is not None and instructions is not None:
        row["ipc"] = instructions / cycles if cycles else 0.0
    mappings = {
        "branches": "branches_per_frame",
        "L1-dcache-loads": "l1d_loads_per_frame",
        "L1-dcache-load-misses": "l1d_misses_per_frame",
        "l2_request_g1.all_no_prefetch": "l2_requests_per_frame",
        "l2_cache_req_stat.ic_dc_miss_in_l2": "l2_misses_per_frame",
        "l1_dtlb_misses": "l1_dtlb_misses_per_frame",
        "l2_dtlb_misses": "l2_dtlb_misses_per_frame",
        "page-faults": "page_faults",
        "minor-faults": "minor_faults",
        "major-faults": "major_faults",
        "context-switches": "context_switches",
        "cpu-migrations": "cpu_migrations",
    }
    for event, field in mappings.items():
        if event in counters:
            divisor = 1 if field in {
                "page_faults",
                "minor_faults",
                "major_faults",
                "context_switches",
                "cpu_migrations",
            } else samples
            row[field] = counters[event] / divisor
    for numerator, denominator, field in (
        ("branch-misses", "branches", "branch_miss_rate"),
        ("L1-dcache-load-misses", "L1-dcache-loads", "l1d_miss_rate"),
        ("l2_cache_req_stat.ic_dc_miss_in_l2", "l2_request_g1.all_no_prefetch", "l2_miss_rate"),
        ("l2_dtlb_misses", "l1_dtlb_misses", "l2_dtlb_miss_rate"),
    ):
        if numerator in counters and denominator in counters:
            row[field] = counters[numerator] / counters[denominator]


def run_case(paths, environment, output, temporary, case, cpus, samples,
             warmup, interval_ns):
    repetition, group, source_idle, sink_idle, priority = case
    mode = f"{source_idle}-{sink_idle}"
    stem = f"{mode}-rt{priority}-{group}-rep{repetition}"
    name = f"bench-{os.getpid()}-{stem}"
    latency_path = output / f"latency-{stem}.csv"
    raw_stat = output / f"stat-{stem}.csv"
    perf_data = temporary / f"perf-{stem}.data"
    log_path = output / f"client-{stem}.log"
    command = [
        str(paths["client"]),
        name,
        source_idle,
        sink_idle,
        "benchmark",
        str(warmup),
        str(samples),
        str(interval_ns),
        str(cpus[0]),
        str(cpus[1]),
        str(priority),
        str(latency_path),
    ]
    profile = group != "none"
    sample_cycles = group == "cycles"
    child_environment = environment.copy()
    fds = []
    if profile:
        ready_r, ready_w = os.pipe()
        start_r, start_w = os.pipe()
        done_r, done_w = os.pipe()
        finish_r, finish_w = os.pipe()
        fds = [ready_r, ready_w, start_r, start_w, done_r, done_w, finish_r, finish_w]
        child_environment.update(
            {
                "PW_BENCHMARK_PROFILE_READY_FD": str(ready_w),
                "PW_BENCHMARK_PROFILE_START_FD": str(start_r),
                "PW_BENCHMARK_PROFILE_DONE_FD": str(done_w),
                "PW_BENCHMARK_PROFILE_FINISH_FD": str(finish_r),
            }
        )
    client = subprocess.Popen(
        command,
        env=child_environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        pass_fds=(ready_w, start_r, done_w, finish_r) if profile else (),
    )
    if profile:
        for fd in (ready_w, start_r, done_w, finish_r):
            os.close(fd)
            fds.remove(fd)
    perf = None
    perf_command = None
    perf_output = ""
    try:
        link_streams(paths, environment, client, name)
        if profile:
            thread_size = struct.calcsize("ii")
            source_tid, sink_tid = struct.unpack("ii", wait_read(ready_r, thread_size))
            control_r, control_w = os.pipe()
            ack_r, ack_w = os.pipe()
            fds.extend((control_w, ack_r))
            if sample_cycles:
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
                    "999",
                    "--mmap-pages",
                    "8192",
                    "--call-graph",
                    "dwarf,8192",
                    "--tid",
                    f"{source_tid},{sink_tid}",
                ]
            else:
                perf_command = [
                    "perf",
                    "stat",
                    "--no-big-num",
                    "-x",
                    ";",
                    "--output",
                    str(raw_stat),
                    "--delay=-1",
                    f"--control=fd:{control_r},{ack_w}",
                    "-e",
                    EVENT_GROUPS[group],
                    "-t",
                    f"{source_tid},{sink_tid}",
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
                raise BenchmarkFailure("perf did not acknowledge enable")
            os.write(start_w, b"\x01")
            wait_read(done_r, 1, timeout=120)
            os.write(control_w, b"disable\n")
            if not valid_perf_ack(wait_line(ack_r)):
                raise BenchmarkFailure("perf did not acknowledge disable")
            os.write(finish_w, b"\x01")
        client_output = client.communicate(timeout=120)[0]
        if perf is not None:
            perf_stdout, perf_stderr = perf.communicate(timeout=30)
            perf_output = f"perf-stdout:\n{perf_stdout}perf-stderr:\n{perf_stderr}"
            if perf.returncode != 0:
                raise BenchmarkFailure(f"perf failed: {perf_output}")
            if sample_cycles and "lost" in perf_stderr.lower():
                raise BenchmarkFailure(f"perf lost cycle samples: {perf_output}")
        log_path.write_text(client_output, encoding="utf-8")
        if client.returncode != 0:
            raise BenchmarkFailure(f"{stem} failed: {client_output}")
        values = parse_result(client_output)
        row = {
            "repetition": repetition,
            "group": group,
            "source_idle": source_idle,
            "sink_idle": sink_idle,
            "rt_priority": priority,
            "samples": samples,
            "warmup": warmup,
            "interval_ns": interval_ns,
            "measurement_ns": values["measurement-ns"],
            "frames_per_second": samples * 1e9 / values["measurement-ns"],
            "source_tid": values["source-tid"],
            "source_cpu": values["source-cpu"],
            "source_policy": values["source-policy"],
            "source_priority": values["source-priority"],
            "sink_tid": values["sink-tid"],
            "sink_cpu": values["sink-cpu"],
            "sink_policy": values["sink-policy"],
            "sink_priority": values["sink-priority"],
            "latency_csv": latency_path.name,
            "raw_perf_stat": raw_stat.name if profile and not sample_cycles else "",
        }
        row.update(summarize_latencies(latency_path, samples))
        if sample_cycles:
            reports = {
                "cycles_report": output
                / f"cycles-{mode}-rt{priority}-rep{repetition}-report.txt",
                "cycles_children_report": output
                / f"cycles-{mode}-rt{priority}-rep{repetition}-children-report.txt",
                "cycles_buildids": output
                / f"cycles-{mode}-rt{priority}-rep{repetition}-buildids.txt",
            }
            report_commands = {
                "cycles_report": [
                    "perf", "report", "--input", str(perf_data), "--stdio",
                    "--no-children", "--percent-limit", "0.1",
                    "--sort", "comm,dso,symbol",
                ],
                "cycles_children_report": [
                    "perf", "report", "--input", str(perf_data), "--stdio",
                    "--children", "--percent-limit", "0.1",
                    "--sort", "comm,dso,symbol",
                ],
                "cycles_buildids": [
                    "perf", "buildid-list", "--input", str(perf_data), "--with-hits",
                ],
            }
            for field, command_line in report_commands.items():
                report = subprocess.run(
                    command_line,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    check=False,
                    timeout=30,
                )
                reports[field].write_text(report.stdout, encoding="utf-8")
                if report.returncode != 0:
                    raise BenchmarkFailure(f"could not create {field}: {report.stdout}")
                row[field] = reports[field].name
            perf_data.unlink()
        elif profile:
            counters, coverage = parse_perf_stat(raw_stat)
            row["minimum_counter_coverage_percent"] = coverage
            add_counters(row, counters, samples)
        expected_policy = os.SCHED_FIFO if priority else os.SCHED_OTHER
        if (
            values["source-cpu"] != cpus[0]
            or values["sink-cpu"] != cpus[1]
            or values["source-policy"] != expected_policy
            or values["sink-policy"] != expected_policy
            or values["source-priority"] != priority
            or values["sink-priority"] != priority
        ):
            raise BenchmarkFailure(f"loop placement or scheduler mismatch: {row}")
        if int(row.get("cpu_migrations", 0) or 0) != 0:
            raise BenchmarkFailure(f"profiled loops migrated: {row}")
        log = (
            f"$ {' '.join(command)}\n"
            + (f"$ {' '.join(perf_command)}\n" if perf_command else "")
            + f"client-output:\n{client_output}{perf_output}\n"
        )
        return row, log
    finally:
        for fd in fds:
            try:
                os.close(fd)
            except OSError:
                pass
        if client.poll() is None:
            client.kill()
            client.wait()
        if perf is not None and perf.poll() is None:
            perf.kill()
            perf.wait()


def write_csv(path, rows):
    fields = []
    for row in rows:
        for field in row:
            if field not in fields:
                fields.append(field)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fields)
        writer.writeheader()
        writer.writerows(rows)


def write_aggregates(rows, output):
    keys = ("group", "source_idle", "sink_idle", "rt_priority")
    excluded = set(keys) | {
        "repetition",
        "samples",
        "warmup",
        "interval_ns",
        "source_tid",
        "sink_tid",
        "source_cpu",
        "sink_cpu",
        "source_policy",
        "sink_policy",
        "source_priority",
        "sink_priority",
        "latency_csv",
        "raw_perf_stat",
        "cycles_report",
        "cycles_children_report",
        "cycles_buildids",
    }
    metrics = []
    for row in rows:
        for field, value in row.items():
            if (
                field not in excluded
                and field not in metrics
                and isinstance(value, (int, float))
            ):
                metrics.append(field)
    groups = {}
    for row in rows:
        key = tuple(row[name] for name in keys)
        groups.setdefault(key, []).append(row)
    aggregate_rows = []
    for key, members in sorted(groups.items()):
        result = dict(zip(keys, key))
        result["repetitions"] = len(members)
        for metric in metrics:
            values = sorted(float(member[metric]) for member in members if metric in member)
            if values:
                result[f"{metric}_min"] = values[0]
                result[f"{metric}_median"] = statistics.median(values)
                result[f"{metric}_max"] = values[-1]
        aggregate_rows.append(result)
    write_csv(output / "aggregate.csv", aggregate_rows)


def write_manifest(output):
    with (output / "SHA256SUMS").open("w", encoding="utf-8") as stream:
        for path in sorted(output.iterdir()):
            if path.name != "SHA256SUMS" and path.is_file():
                stream.write(f"{sha256_file(path)}  {path.name}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("daemon", type=Path)
    parser.add_argument("link", type=Path)
    parser.add_argument("client", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--samples", type=int, default=10_000)
    parser.add_argument("--warmup", type=int, default=2_000)
    parser.add_argument("--interval-ns", type=int, default=100_000)
    parser.add_argument("--source-cpu", type=int)
    parser.add_argument("--sink-cpu", type=int)
    parser.add_argument("--rt-priority", type=int, action="append", default=[])
    parser.add_argument("--mode", choices=DEFAULT_MODES, action="append", default=[])
    parser.add_argument("--group", choices=EVENT_GROUPS, action="append", default=[])
    parser.add_argument("--sample-cycles", action="store_true")
    parser.add_argument("--seed", type=int, default=20260825)
    args = parser.parse_args()

    paths = {
        "daemon": args.daemon.resolve(strict=True),
        "link": args.link.resolve(strict=True),
        "client": args.client.resolve(strict=True),
    }
    output = args.output.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise SystemExit(f"refusing to overwrite output path: {output}")
    output.mkdir(parents=True, exist_ok=True)
    if args.repetitions <= 0 or args.samples <= 0 or args.warmup <= 0:
        raise SystemExit("repetitions, samples, and warmup must be positive")
    source_root = Path(__file__).resolve().parents[2]
    build_root = paths["client"].parents[2]
    cpus, allowed = select_cpus(args.source_cpu, args.sink_cpu)
    priorities = args.rt_priority or [0, 88]
    modes = args.mode or list(DEFAULT_MODES)
    if args.sample_cycles and args.group:
        raise SystemExit("--sample-cycles and --group are separate campaigns")
    groups = ["cycles"] if args.sample_cycles else (args.group or ["none"])
    environment = os.environ.copy()
    environment.update(
        {
            "PIPEWIREAO_SPA_PLUGIN_DIR": str(build_root / "spa/plugins"),
            "PIPEWIREAO_CONFIG_DIR": str(build_root / "src/daemon"),
            "PIPEWIREAO_MODULE_DIR": str(build_root / "src/modules"),
            "LD_LIBRARY_PATH": str(build_root / "src/pipewire"),
            "PIPEWIREAO_LOG_SYSTEMD": "false",
        }
    )
    capture_environment(output, source_root, build_root, paths, cpus, allowed)
    snapshot_system(output / "system-before.txt")
    cases = []
    for repetition in range(1, args.repetitions + 1):
        repeated = []
        for group in groups:
            for mode in modes:
                source_idle, sink_idle = mode.split("-", 1)
                if mode.startswith("busy-spin-"):
                    source_idle = "busy-spin"
                    sink_idle = mode.removeprefix("busy-spin-")
                for priority in priorities:
                    repeated.append(
                        (repetition, group, source_idle, sink_idle, priority)
                    )
        random.Random(args.seed + repetition).shuffle(repeated)
        cases.extend(repeated)

    started = datetime.datetime.now(datetime.timezone.utc)
    rows = []
    logs = []
    with tempfile.TemporaryDirectory(prefix="pwao-remote-benchmark.") as temporary:
        temporary = Path(temporary)
        runtime = temporary / "run"
        runtime.mkdir()
        environment.update(
            {
                "PIPEWIREAO_RUNTIME_DIR": str(runtime),
                "PIPEWIREAO_REMOTE": "pipewire-ao-0",
            }
        )
        daemon_log_path = output / "daemon.log"
        with daemon_log_path.open("w+", encoding="utf-8") as daemon_log:
            daemon_command = [
                str(paths["daemon"]),
                "-P",
                "{ context.data-loops = [ "
                "{ loop.name=daemon-event loop.class=data.rt loop.idle=eventfd } "
                "] }",
            ]
            daemon = subprocess.Popen(
                daemon_command,
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
                logs.append(f"$ {' '.join(daemon_command)}\n")
                for index, case in enumerate(cases, 1):
                    print(f"[{index}/{len(cases)}] {case}", flush=True)
                    row, log = run_case(
                        paths,
                        environment,
                        output,
                        temporary,
                        case,
                        cpus,
                        args.samples,
                        args.warmup,
                        args.interval_ns,
                    )
                    rows.append(row)
                    logs.append(log)
                    (output / "commands.log").write_text(
                        "\n".join(logs), encoding="utf-8"
                    )
                    time.sleep(0.05)
                if daemon.poll() is not None:
                    raise BenchmarkFailure("daemon exited during campaign")
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                    try:
                        daemon.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        daemon.kill()
                        daemon.wait()
    ended = datetime.datetime.now(datetime.timezone.utc)
    write_csv(output / "summary.csv", rows)
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
        "interval_ns": args.interval_ns,
        "source_cpu": cpus[0],
        "sink_cpu": cpus[1],
        "rt_priorities": priorities,
        "modes": modes,
        "event_groups": {
            name: ("cycles:u at 999 Hz with 8 KiB DWARF stacks"
                   if name == "cycles" else EVENT_GROUPS[name])
            for name in groups
            if name != "none"
        },
        "measurement_scope": (
            "trigger call through source and sink process callbacks in two "
            "exported-node data loops linked by an isolated daemon"
        ),
        "latency_model": "closed-loop, one frame in flight",
    }
    (output / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    write_manifest(output)
    print(f"wrote {len(rows)} cells to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
