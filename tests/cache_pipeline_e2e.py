#!/usr/bin/env python3
"""Validate MLIR trace, runner placement and cache statistics end to end."""
from __future__ import annotations

import argparse
from collections import Counter
import json
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples"))
sys.path.insert(0, str(ROOT / "scripts"))
from trace_demo import VARIANTS, Variant, compare_traces, compile_variant  # noqa: E402


def check(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def replay(runner: Path, trace: dict, trace_path: Path, output: Path,
           size: int, ways: int) -> tuple[dict, dict]:
    subprocess.run([str(runner), "--trace", str(trace_path), "--output-dir", str(output),
                    "--size-bytes", str(size), "--line-size-bytes", "64",
                    "--associativity", str(ways), "--replacement-policy", "lru"], check=True)
    stats = json.loads((output / "statistics.json").read_text())
    timeline = json.loads((output / "events.json").read_text())
    buffers = {entry["memref_id"]: int(entry["base_address"], 16)
               for entry in stats["buffers"]}
    check(stats["processed"]["trace_events"] == len(trace["events"]), "trace event count")
    check(len(timeline["events"]) == len(trace["events"]), "timeline event count")
    for sequence, (source, event) in enumerate(zip(trace["events"], timeline["events"], strict=True)):
        check(source["sequence"] == sequence and event["trace_event"] == source,
              f"event {sequence}: source trace mismatch")
        check(int(event["address"], 16) == buffers[source["memref_id"]] + source["byte_offset"],
              f"event {sequence}: model base or offset mismatch")
        check(event["kind"] == source["kind"] and event["size_bytes"] == source["size_bytes"],
              f"event {sequence}: kind or size mismatch")
        check(source["size_bytes"] == 4 and int(event["address"], 16) % 64 <= 60,
              f"event {sequence}: unsupported comparison access")
    hits = sum(line["hit"] for event in timeline["events"] for line in event["lines"])
    misses = sum(not line["hit"] for event in timeline["events"] for line in event["lines"])
    check((hits, misses) == (stats["totals"]["hits"], stats["totals"]["misses"]),
          "timeline/statistics hit/miss mismatch")
    for kind in ("read", "write"):
        subset = [event for event in timeline["events"] if event["kind"] == kind]
        kind_hits = sum(line["hit"] for event in subset for line in event["lines"])
        kind_misses = sum(not line["hit"] for event in subset for line in event["lines"])
        check(stats["totals"][kind + "_hits"] == kind_hits and
              stats["totals"][kind + "_misses"] == kind_misses and
              stats["totals"][kind + "_line_accesses"] == kind_hits + kind_misses,
              kind + " totals disagree with events")
    return stats, timeline


def compare_external(launcher: Path, timeline: dict, stats: dict, output: Path,
                     compare_prefixes: bool, size: int, ways: int) -> None:
    def entry(kind: int, size: int, value: int) -> bytes:
        return struct.pack("<HHQ", kind, size, value)
    events = timeline["events"]
    prefixes = range(1, len(events) + 1) if compare_prefixes else [len(events)]
    for count in prefixes:
        records = [(25, 0, 8), (28, 12, 8), (28, 9, 0), (22, 4, 1),
                   (24, 4, 1), (28, 10, 64), (28, 18, 4096), (28, 2, 1), (28, 3, 0)]
        for event in events[:count]:
            records.append((10, 4, 0x400000))
            records.append((0 if event["kind"] == "read" else 1, event["size_bytes"],
                            int(event["address"], 16)))
        records.extend([(23, 0, 1), (26, 0, 0)])
        path = output / f"drcachesim-{count}.trace"
        path.write_bytes(b"".join(entry(*record) for record in records))
        command = [str(launcher), "-infile", str(path), "-cores", "1", "-line_size", "64",
                   "-L1D_size", str(size), "-L1D_assoc", str(ways), "-replace_policy", "LRU",
                   "-data_prefetcher", "none", "-sim_refs", "1000000"]
        result = subprocess.run(command, check=True, text=True, capture_output=True)
        log = result.stdout + result.stderr
        (output / f"drcachesim-{count}.log").write_text(log)
        pattern = (rf"L1D0 \(size={size}, assoc={ways}, block=64, LRU\) stats:\s*"
                   r"Hits:\s*([\d,]+)\s*Misses:\s*([\d,]+)")
        match = re.search(pattern, log)
        check(match is not None, "drcachesim output lacks expected L1D configuration")
        external = tuple(int(value.replace(",", "")) for value in match.groups())
        hits = sum(line["hit"] for event in events[:count] for line in event["lines"])
        misses = sum(not line["hit"] for event in events[:count] for line in event["lines"])
        expected = (hits, misses)
        check(external == expected, f"drcachesim prefix {count}: {external} != runner {expected}")
        if count == len(events):
            check(expected == (stats["totals"]["hits"], stats["totals"]["misses"]),
                  "drcachesim prefix does not match totals")


def validate_small(trace: dict) -> None:
    expected = [("write", 0), ("read", 0), ("write", 64),
                ("read", 128), ("read", 64), ("write", 4)]
    actual = [(e["kind"], e["byte_offset"]) for e in trace["events"]]
    check(actual == expected, f"small kernel semantic accesses: {actual}")
    check(all(e["memref_id"] == 0 and e["size_bytes"] == 4 and
              e["address_kind"] == "memref_offset" for e in trace["events"]),
          "small kernel event metadata")


def validate_matmul(trace: dict, name: str) -> None:
    expected = Counter()
    for row in range(16):
        for col in range(16):
            offset = (row * 16 + col) * 4
            for memref in (0, 1):
                expected[(memref, "read", offset, 4)] = 16
            expected[(2, "read", offset, 4)] = 16
            expected[(2, "write", offset, 4)] = 16
    actual = Counter((e["memref_id"], e["kind"], e["byte_offset"], e["size_bytes"])
                     for e in trace["events"])
    check(actual == expected, "matmul semantic access multiset differs from 16x16 matmul")
    if name == "baseline":
        expected_order = []
        for i in range(16):
            for j in range(16):
                for k in range(16):
                    c = (i * 16 + j) * 4
                    expected_order.extend([(0, "read", (i * 16 + k) * 4, 4),
                                           (1, "read", (k * 16 + j) * 4, 4),
                                           (2, "read", c, 4), (2, "write", c, 4)])
        actual_order = [(e["memref_id"], e["kind"], e["byte_offset"], e["size_bytes"])
                        for e in trace["events"]]
        check(actual_order == expected_order, "baseline matmul access order differs from i-j-k")


def execute(args: argparse.Namespace, root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    common = dict(output_root=root, tool_dir=args.mlir_bin, plugin=args.plugin, runtime=args.runtime)
    small = compile_variant(Variant("small", None), payload=ROOT / "tests/fixtures/cache_pipeline.mlir", **common)
    validate_small(small)
    variants = [("small", small)]
    for variant in VARIANTS:
        trace = compile_variant(variant, payload=ROOT / "examples/workloads/matmul.mlir", **common)
        validate_matmul(trace, variant.name)
        variants.append((variant.name, trace))
    compare_traces(dict(variants[1:]))
    for name, trace in variants:
        out = root / "simulation" / name
        size, ways = (128, 2) if name == "small" else (512, 1)
        stats, timeline = replay(args.runner, trace, root / name / "trace.json", out, size, ways)
        if name == "small":
            check(len(stats["buffers"]) == 1 and stats["buffers"][0]["memref_id"] == 0 and
                  int(stats["buffers"][0]["base_address"], 16) == 0x10000000,
                  "small kernel model buffer placement")
            outcomes = [event["lines"][0]["hit"] for event in timeline["events"]]
            check(outcomes == [False, True, False, False, True, False],
                  f"small kernel outcomes: {outcomes}")
            check((stats["totals"]["hits"], stats["totals"]["misses"]) == (2, 4),
                  "small kernel totals")
        if args.dynamorio_launcher:
            compare_external(args.dynamorio_launcher, timeline, stats, out, name == "small", size, ways)
        totals = stats["totals"]
        print("{}: trace and runner verified; hits={} misses={}".format(
            name, totals["hits"], totals["misses"]))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("mlir-bin", "plugin", "runtime", "runner"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--dynamorio-launcher", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    for name in ("mlir_bin", "plugin", "runtime", "runner", "dynamorio_launcher"):
        if getattr(args, name) is not None:
            setattr(args, name, getattr(args, name).resolve())
    if args.output_dir:
        output = args.output_dir.resolve()
        output.mkdir(parents=True, exist_ok=False)
        execute(args, output)
    else:
        with tempfile.TemporaryDirectory(prefix="cachescope-cache-pipeline-") as directory:
            execute(args, Path(directory))


if __name__ == "__main__":
    main()
