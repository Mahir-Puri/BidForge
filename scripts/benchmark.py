#!/usr/bin/env python3
"""Release-build measurements with fresh servers and recorded inputs."""
import json
import platform
from pathlib import Path
import statistics
import subprocess
import threading
import time
from integration import ROOT, Server

OUT = ROOT / "results"
LOADGEN = ROOT / "tools/loadgen/loadgen"


def run(*args):
    subprocess.run([str(a) for a in args], check=True, cwd=ROOT)


def measure_http(config, mode, label, **flags):
    options = flags.pop("server", {})
    with Server(config, mode, **options) as server:
        samples = []
        finished = threading.Event()

        def sample_resources():
            while not finished.is_set():
                result = subprocess.run(["ps", "-o", "rss=", "-p", str(server.process.pid)], capture_output=True, text=True)
                if result.returncode == 0 and result.stdout.strip():
                    samples.append(int(result.stdout.strip()))
                finished.wait(0.05)

        monitor = threading.Thread(target=sample_resources)
        monitor.start()
        path = OUT / f"{label}.json"
        try:
            args = [LOADGEN, "run", "-url", server.url, "-seed", "17", "-output", path]
            for key, value in flags.items():
                args += ["-" + key.replace("_", "-"), str(value)]
            run(*args)
        finally:
            finished.set()
            monitor.join()
        data = json.loads(path.read_text())
        assert data["invariant_violations"] == 0
        assert data["duplicate_decision_mismatches"] == 0
        data["sampled_peak_server_rss_kib"] = max(samples, default=0)
        data["mode"] = mode
        data["label"] = label
        data["flags"] = flags
        data["server_options"] = options
        # Keep full snapshots in raw results; the checked-in summary is compact.
        for key in ["stats_before", "stats_after"]:
            snapshot = data[key]
            snapshot.pop("campaigns", None)
        return data


def main():
    OUT.mkdir(exist_ok=True)
    summary = {"machine": platform.platform(), "architecture": platform.machine(),
               "compiler": subprocess.check_output(["clang++", "--version"], text=True).splitlines()[0],
               "build": "Release", "seed": 17, "matching": [], "http": []}
    try:
        summary["cpu"] = subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
        summary["memory_bytes"] = int(subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True))
    except (OSError, subprocess.CalledProcessError):
        summary["cpu"] = platform.processor()
    for count in [100, 1000]:
        for categories in [1, 100]:
            config_path = OUT / f"campaigns-{count}-{categories}.json"
            run(LOADGEN, "generate", "-campaigns", count, "-categories", categories, "-seed", 17, "-budget", 50000000000, "-output", config_path)
            raw = subprocess.check_output([str(ROOT / "build/match_bench"), str(config_path), str(categories), "2000"], text=True)
            (OUT / f"matching-{count}-{categories}.jsonl").write_text(raw)
            rows = [json.loads(line) for line in raw.splitlines()]
            summary["matching"].extend(rows)
            if count != 1000:
                continue
            config = json.loads(config_path.read_text())
            for mode in ["baseline", "indexed"]:
                # Warm up code and tool startup separately, then reset accounting.
                measure_http(config, mode, f"warmup-{categories}-{mode}", requests=200, categories=categories, concurrency=8, duplicate_percent=0)
                for repeat in range(3):
                    summary["http"].append(measure_http(config, mode, f"http-{categories}-{mode}-{repeat}",
                        requests=1000, categories=categories, concurrency=8, duplicate_percent=20,
                        duplicate_outcome_percent=20, missing_percent=10))
    # With ample budgets and generous deadlines, ordering must not change decisions.
    for categories in [1, 100]:
        baseline = json.loads((OUT / f"http-{categories}-baseline-0.json").read_text())
        for mode in ["baseline", "indexed"]:
            for repeat in range(3):
                result = json.loads((OUT / f"http-{categories}-{mode}-{repeat}.json").read_text())
                for key in ["unique_wins", "unique_losses", "unique_decisions", "omitted_outcomes"]:
                    assert result[key] == baseline[key], (mode, categories, key)
                assert result["stats_after"]["campaigns"] == baseline["stats_after"]["campaigns"]
    config = json.loads((OUT / "campaigns-1000-100.json").read_text())
    summary["http"].append(measure_http(config, "indexed", "overload-queue", requests=1000, concurrency=128,
        categories=100, tmax=1, duplicate_percent=0, server={"workers": 1, "queue": 2}))
    summary["http"].append(measure_http(config, "indexed", "overload-history", requests=500, concurrency=32,
        categories=100, duplicate_percent=0, server={"history": 20}))
    summary["http"].append(measure_http(config, "indexed", "delayed-missing", requests=200, concurrency=8,
        categories=100, duplicate_percent=100, missing_percent=50, outcome_delay="2ms", duplicate_outcome_percent=100))
    path = ROOT / "docs/measurements.json"
    path.write_text(json.dumps(summary, indent=2) + "\n")
    print(f"Saved {path}")


if __name__ == "__main__":
    main()
