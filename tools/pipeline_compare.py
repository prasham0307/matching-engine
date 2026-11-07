#!/usr/bin/env python3
"""Interleaved comparison of the lob_pipeline queue variants against lob_latency.

Each round runs every variant once, and the variant that goes first rotates from round to round,
so no variant always benefits from (or suffers) being first. Prints median, min and max throughput
per variant, whether each variant's range overlaps the default's, and a paired view: in how many
rounds the variant beat the default measured in the same round, and the median of those
per-round ratios. The paired view removes the machine-state drift that is common to the runs of
one round. Also checks that every run produced one and the same checksum: threads must not change
results.

Usage: pipeline_compare.py [--rounds 7] [--ops 5000000] [--warmup 200000] [--seed 1]
                           [--build-dir build-release] [--capacity 1024]
"""
import argparse
import statistics
import subprocess
import sys


def parse_result(text):
    for line in text.splitlines():
        if line.startswith("RESULT"):
            return dict(kv.split("=", 1) for kv in line.split()[1:])
    raise RuntimeError("no RESULT line in output:\n" + text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=7)
    ap.add_argument("--ops", type=int, default=5_000_000)
    ap.add_argument("--warmup", type=int, default=200_000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--capacity", type=int, default=1024)
    ap.add_argument("--build-dir", default="build-release")
    a = ap.parse_args()

    common = ["--ops", str(a.ops), "--warmup", str(a.warmup), "--seed", str(a.seed)]
    pipeline = f"{a.build_dir}/bench/lob_pipeline"
    runs = [
        ("single-thread (lob_latency)", [f"{a.build_dir}/bench/lob_latency", *common, "--pin-core", "2"]),
        ("default: cached, padded", [pipeline, *common, "--capacity", str(a.capacity), "--variant", "default"]),
        ("uncached, padded", [pipeline, *common, "--capacity", str(a.capacity), "--variant", "uncached"]),
        ("cached, unpadded", [pipeline, *common, "--capacity", str(a.capacity), "--variant", "unpadded"]),
        ("batch pop of 32", [pipeline, *common, "--capacity", str(a.capacity), "--variant", "batch32"]),
    ]
    names = [n for n, _ in runs]
    results = {n: [] for n in names}
    full = {n: [] for n in names}
    empty = {n: [] for n in names}
    checksums = set()

    for rnd in range(a.rounds):
        order = runs[rnd % len(runs):] + runs[:rnd % len(runs)]
        line = []
        for name, cmd in order:
            out = subprocess.run(cmd, capture_output=True, text=True)
            if out.returncode != 0:
                sys.exit(f"{name} failed with exit {out.returncode}:\n{out.stderr}")
            r = parse_result(out.stdout)
            results[name].append(float(r["ops_per_sec"]))
            checksums.add(r["checksum"])
            if "full_episodes" in r:
                full[name].append(int(r["full_episodes"]))
                empty[name].append(int(r["empty_episodes"]))
            line.append(f"{name.split(':')[0].split(' (')[0][:9]} {float(r['ops_per_sec']) / 1e6:5.2f}M")
        print(f"round {rnd + 1}: " + " | ".join(line), flush=True)

    base = results["default: cached, padded"]
    print()
    print(f"{'variant':<30}{'median':>10}{'min':>9}{'max':>9}{'vs default':>12}  {'range vs default':<30}"
          f"{'paired (same round)':<26}{'full eps':>10}{'empty eps':>11}")
    for n in names:
        v = results[n]
        med = statistics.median(v)
        change = 100 * (med - statistics.median(base)) / statistics.median(base)
        if n == "default: cached, padded":
            verdict = "(the reference)"
        elif n.startswith("single"):
            verdict = "n/a (different harness)"
        elif min(v) > max(base):
            verdict = "above: ranges do not overlap"
        elif max(v) < min(base):
            verdict = "below: ranges do not overlap"
        else:
            verdict = "within the spread"
        ratios = [x / y for x, y in zip(v, base)]
        wins = sum(r > 1 for r in ratios)
        paired = "-" if n == "default: cached, padded" else f"{wins}/{len(ratios)} faster, x{statistics.median(ratios):.2f}"
        fe = f"{statistics.median(full[n]):>10,.0f}" if full[n] else f"{'-':>10}"
        ee = f"{statistics.median(empty[n]):>11,.0f}" if empty[n] else f"{'-':>11}"
        print(f"{n:<30}{med / 1e6:>9.2f}M{min(v) / 1e6:>8.2f}M{max(v) / 1e6:>8.2f}M{change:>+11.1f}%  {verdict:<30}{paired:<26}{fe}{ee}")
    spread = (max(base) - min(base)) / statistics.median(base) * 100
    print(f"\nrun-to-run spread of the default over {a.rounds} rounds: {spread:.1f}% of its median")
    print(f"checksums seen across all {a.rounds * len(runs)} runs: {sorted(checksums)} "
          f"({'identical, as required' if len(checksums) == 1 else 'MISMATCH'})")


if __name__ == "__main__":
    main()
