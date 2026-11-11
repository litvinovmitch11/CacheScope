#!/usr/bin/env python3
"""Compare L1D outcomes on synthetic traces with DynamoRIO drcachesim."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x10000000
CASES = {
    "lru_reads": [("R", n) for n in (0, 64, 0, 128, 0, 64)],
    "write_allocate": [("W", 0), ("R", 0), ("W", 64), ("R", 128),
                       ("W", 0), ("R", 64), ("W", 128)],
    "repeated_lines": [("R", 1), ("W", 63), ("R", 64), ("W", 127),
                       ("R", 192), ("R", 2)],
}
# trace_entry_t is <type:uint16, size:uint16, value:uint64> for 64-bit traces.
# Values correspond to DynamoRIO trace_entry.h, trace format version 8.
def write_trace(path, events):
    records = [(25, 0, 8), (28, 12, 8), (28, 9, 0), (22, 4, 1),
               (24, 4, 1), (28, 10, 64), (28, 18, 4096),
               (28, 2, 1), (28, 3, 0)]
    for kind, offset in events:
        records.extend([(10, 4, 0x400000),
                        (0 if kind == "R" else 1, 1, BASE + offset)])
    records.extend([(23, 0, 1), (26, 0, 0)])
    path.write_bytes(b"".join(struct.pack("<HHQ", *entry) for entry in records))


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, capture_output=True, **kwargs)


def parse_l1d(output):
    match = re.search(r"L1D0 \(size=128, assoc=2, block=64, LRU\) stats:\s*"
                      r"Hits:\s*([\d,]+)\s*Misses:\s*([\d,]+)", output)
    if not match:
        raise RuntimeError("drcachesim output lacks expected L1D configuration/counters")
    return tuple(int(value.replace(",", "")) for value in match.groups())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dynamorio", type=Path, required=True,
                        help="DynamoRIO root containing tools/bin64/drmemtrace_launcher")
    parser.add_argument("--output", type=Path, required=True,
                        help="new directory for input traces and raw results")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    launcher = (args.dynamorio / "tools/bin64/drmemtrace_launcher").resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    commands = []
    all_results = {}
    with tempfile.TemporaryDirectory(prefix="cachescope-drcachesim-") as temp:
        binary = Path(temp) / "replay"
        compile_command = [args.cxx, "-std=c++17", "-O2", "-I", str(ROOT / "simulator/include"),
                           str(ROOT / "simulator/tests/drcachesim_replay.cpp"),
                           *[str(ROOT / "simulator/lib" / name) for name in
                             ("SetAssociativeCacheLevel.cpp", "LRU.cpp", "FIFO.cpp", "Random.cpp", "cache_hierarchy.cpp")],
                           "-o", str(binary)]
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
            results = []
            for index in range(1, len(events) + 1):
                trace = case_dir / f"prefix-{index:02}.trace"
                write_trace(trace, events[:index])
                command = [str(launcher), "-infile", str(trace), "-cores", "1", "-line_size", "64",
                           "-L1D_size", "128", "-L1D_assoc", "2", "-replace_policy", "LRU",
                           "-data_prefetcher", "none", "-sim_refs", "1000"]
                commands.append(command)
                process = run(command)
                raw = process.stdout + process.stderr
                (case_dir / f"prefix-{index:02}.log").write_text(raw)
                dr = parse_l1d(raw)
                cs = tuple(map(int, rows[index - 1].split()))
                if len(cs) != 2 or sum(cs) != index or sum(dr) != index or dr != cs:
                    raise RuntimeError(f"{name} prefix {index}: CacheScope={cs}, drcachesim={dr}")
                results.append({"prefix": index, "hits": cs[0], "misses": cs[1]})
            all_results[name] = results
    report = {"dynamorio_root": str(args.dynamorio.resolve()),
              "launcher_sha256": hashlib.sha256(launcher.read_bytes()).hexdigest(),
              "compiler": run([args.cxx, "--version"]).stdout.splitlines()[0],
              "compile_command": compile_command,
              "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in [ROOT / "simulator/tests/drcachesim_replay.cpp",
                                             *[ROOT / "simulator/lib" / name for name in
                                               ("SetAssociativeCacheLevel.cpp", "LRU.cpp", "FIFO.cpp", "Random.cpp", "cache_hierarchy.cpp")]]},
              "config": {"size_bytes": 128, "line_size_bytes": 64, "ways": 2,
                         "policy": "LRU", "prefetch": "none", "cores": 1},
              "trace_format_version": 8, "commands": commands, "cases": all_results}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Matched {sum(map(len, all_results.values()))} prefixes; results: {output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"comparison failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout or "", error.stderr or "", file=sys.stderr)
        sys.exit(1)
