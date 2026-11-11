#!/usr/bin/env python3
"""Compare L1D/L2 demand counters with drcachesim's exclusive hierarchy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from compare_drcachesim import BASE, ROOT, run, write_trace

CONFIG = """num_cores 1
line_size 64
skip_refs 0
warmup_refs 0
sim_refs 10000
L1I {
type instruction
core 0
size 128
assoc 2
parent memory
prefetcher none
replace_policy LRU
}
L1D {
type data
core 0
size 128
assoc 2
parent L2
prefetcher none
replace_policy LRU
}
L2 {
size 128
assoc 2
parent memory
exclusive true
prefetcher none
replace_policy LRU
}
"""


def generated():
    state = 0x12345678
    events = []
    for _ in range(32):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        address = ((state >> 16) % 8) * 64 + ((state >> 24) % 64)
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        events.append(("W" if (state >> 16) & 1 else "R", address))
    return events


CASES = {
    "promotion": [("R", offset) for offset in (0, 64, 128, 64, 0)],
    "dirty_migration": [("W", 0), ("R", 64), ("R", 128), ("R", 0),
                        ("W", 192), ("R", 256), ("R", 320), ("R", 0)],
    "generated_seed_12345678": generated(),
}
# Hand-computed cumulative (L1 hits, L1 misses, L2 hits, L2 misses).
GOLDEN = [(0, 1, 0, 1), (0, 2, 0, 2), (0, 3, 0, 3),
          (1, 3, 0, 3), (1, 4, 1, 3)]


def parse_counters(output, name, exclusive):
    suffix = r", exclusive" if exclusive else ""
    pattern = (rf"(?:^|\n)\s*{name} \(size=128, assoc=2, block=64, LRU{suffix}\) stats:\s*"
               r"Hits:\s*([\d,]+)\s*Misses:\s*([\d,]+)")
    matches = re.findall(pattern, output)
    if len(matches) != 1:
        raise RuntimeError(f"expected exactly one {name} statistics block with specified configuration")
    return tuple(int(value.replace(",", "")) for value in matches[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dynamorio", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="new directory for configuration, traces and raw results")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    launcher = (args.dynamorio / "tools/bin64/drmemtrace_launcher").resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    config = output / "exclusive.cfg"
    config.write_text(CONFIG)
    sources = [ROOT / "simulator/tests/drcachesim_exclusive_replay.cpp",
               *[ROOT / "simulator/lib" / name for name in
                 ("SetAssociativeCacheLevel.cpp", "LRU.cpp", "FIFO.cpp", "Random.cpp", "cache_hierarchy.cpp")]]
    results = {}
    commands = []
    with tempfile.TemporaryDirectory(prefix="cachescope-drcachesim-exclusive-") as temp:
        binary = Path(temp) / "replay"
        compile_command = [args.cxx, "-std=c++17", "-O2", "-I", str(ROOT / "simulator/include"),
                           *map(str, sources), "-o", str(binary)]
        run(compile_command)
        for name, events in CASES.items():
            case_dir = output / name
            case_dir.mkdir()
            source = "".join(f"{kind} {BASE + offset}\n" for kind, offset in events)
            (case_dir / "accesses.txt").write_text(source)
            replay = run([str(binary)], input=source).stdout
            (case_dir / "cachescope.txt").write_text(replay)
            rows = replay.splitlines()
            if len(rows) != len(events):
                raise RuntimeError(f"{name}: CacheScope returned an unexpected number of rows")
            results[name] = []
            for index in range(1, len(events) + 1):
                trace = case_dir / f"prefix-{index:02}.trace"
                write_trace(trace, events[:index])
                command = [str(launcher), "-infile", str(trace), "-config_file", str(config)]
                commands.append(command)
                process = run(command)
                raw = process.stdout + process.stderr
                (case_dir / f"prefix-{index:02}.log").write_text(raw)
                dr = parse_counters(raw, "L1D", False) + parse_counters(raw, "L2", True)
                cs = tuple(map(int, rows[index - 1].split()))
                if (len(cs) != 4 or cs[0] + cs[1] != index or
                        cs[2] + cs[3] != cs[1] or dr != cs):
                    raise RuntimeError(f"{name} prefix {index}: CacheScope={cs}, drcachesim={dr}")
                if name == "promotion" and cs != GOLDEN[index - 1]:
                    raise RuntimeError(f"{name} prefix {index}: {cs} != golden {GOLDEN[index - 1]}")
                results[name].append({"prefix": index, "l1_hits": cs[0], "l1_misses": cs[1],
                                      "l2_hits": cs[2], "l2_misses": cs[3]})
    report = {"dynamorio_root": str(args.dynamorio.resolve()),
              "launcher_sha256": hashlib.sha256(launcher.read_bytes()).hexdigest(),
              "compiler": run([args.cxx, "--version"]).stdout.splitlines()[0],
              "compile_command": compile_command, "commands": commands,
              "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in sources + [ROOT / "scripts/compare_drcachesim.py",
                                                       ROOT / "scripts/compare_drcachesim_exclusive.py"]},
              "configuration": CONFIG, "trace_format_version": 8, "cases": results}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Matched {sum(map(len, results.values()))} exclusive hierarchy prefixes; results: {output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"comparison failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout or "", error.stderr or "", file=sys.stderr)
        sys.exit(1)
