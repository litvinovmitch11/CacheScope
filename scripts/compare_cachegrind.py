#!/usr/bin/env python3
"""Compare CacheScope with Cachegrind on controlled native byte accesses."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "reuse": [("R", n) for n in (0, 4096, 0, 8192, 0, 4096)],
    "writes": [("W", 0), ("R", 0), ("W", 4096), ("R", 4096),
               ("W", 8192), ("R", 0), ("R", 8192)],
    "conflict": [("R", n * 4096) for n in (0, 1, 2, 3, 4, 5, 6, 7, 0, 8, 1, 0)],
}
EVENTS = ("Dr", "D1mr", "Dw", "D1mw")
# Hand-calculated final (reads, read misses, writes, write misses).
GOLDEN = {"reuse": (6, 3, 0, 0), "writes": (4, 0, 3, 3),
          "conflict": (12, 10, 0, 0)}


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, capture_output=True, **kwargs)


def kernel_source(events):
    lines = ["#include <valgrind/cachegrind.h>", "#include <stdint.h>",
             "#include <stdio.h>", "#include <stdalign.h>",
             "alignas(4096) static volatile uint8_t data[49152];",
             "__attribute__((noinline)) unsigned kernel(void) {",
             "  unsigned sum = 0;"]
    event_lines = {}
    for kind, offset in events:
        statement = f"sum += data[{offset}];" if kind == "R" else f"data[{offset}] = 1;"
        lines.append("  " + statement)
        event_lines[len(lines)] = kind
    lines += ["  return sum;", "}",
              "int main(void) {", "  CACHEGRIND_START_INSTRUMENTATION;",
              "  unsigned sum = kernel();", "  CACHEGRIND_STOP_INSTRUMENTATION;",
              '  printf("%p %u\\n", (void *)data, sum);', "  return 0;", "}"]
    return "\n".join(lines) + "\n", event_lines


def kernel_counts(path, expected_lines):
    text = path.read_text()
    if "desc: D1 cache:         32768 B, 64 B, 8-way associative" not in text:
        raise RuntimeError("Cachegrind did not report the expected D1 configuration")
    events = None
    current_function = None
    counts = {name: 0 for name in EVENTS}
    seen = set()
    for line in text.splitlines():
        if line.startswith("events:"):
            events = line.split()[1:]
        elif line.startswith("fn="):
            current_function = line[3:]
        elif current_function == "kernel" and re.match(r"^\d+\s", line):
            fields = line.split()
            number = int(fields[0])
            if events is None or len(fields) != len(events) + 1:
                raise RuntimeError("unexpected Cachegrind line format")
            values = dict(zip(events, map(int, fields[1:])))
            if number not in expected_lines:
                if values["Dr"] or values["Dw"]:
                    raise RuntimeError(f"unexpected kernel memory access at line {number}")
                continue
            kind = expected_lines[number]
            if (values["Dr"], values["Dw"]) != ((1, 0) if kind == "R" else (0, 1)):
                raise RuntimeError(f"kernel access count differs at line {number}: {values}")
            if number in seen:
                raise RuntimeError(f"duplicate kernel line {number}")
            seen.add(number)
            for key in EVENTS:
                counts[key] += values[key]
    if seen != set(expected_lines):
        raise RuntimeError(f"missing instrumented kernel lines: {sorted(set(expected_lines) - seen)}")
    return tuple(counts[key] for key in EVENTS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--valgrind", default="valgrind")
    parser.add_argument("--include", type=Path, help="directory containing valgrind/cachegrind.h")
    parser.add_argument("--output", type=Path, required=True, help="new directory for raw results")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    version = run([args.valgrind, "--version"]).stdout.strip()
    sources = [ROOT / "simulator/tests/cachegrind_replay.cpp",
               *[ROOT / "simulator/lib" / name for name in
                 ("SetAssociativeCacheLevel.cpp", "LRU.cpp", "FIFO.cpp", "Random.cpp", "cache_hierarchy.cpp")]]
    results = {}
    with tempfile.TemporaryDirectory(prefix="cachescope-cachegrind-") as temp:
        replay_binary = Path(temp) / "replay"
        compile_command = [args.cxx, "-std=c++17", "-O2", "-I", str(ROOT / "simulator/include"),
                           *map(str, sources), "-o", str(replay_binary)]
        run(compile_command)
        for name, accesses in CASES.items():
            case_dir = output / name
            case_dir.mkdir()
            results[name] = []
            for index in range(1, len(accesses) + 1):
                prefix = accesses[:index]
                source, event_lines = kernel_source(prefix)
                source_path = case_dir / f"prefix-{index:02}.c"
                source_path.write_text(source)
                binary = Path(temp) / "kernel"
                cc_command = [args.cc, "-std=c11", "-O2", "-g", "-fno-pie", "-no-pie", "-fno-stack-protector"]
                if args.include:
                    cc_command += ["-I", str(args.include)]
                cc_command += [str(source_path), "-o", str(binary)]
                run(cc_command)
                cg_path = case_dir / f"prefix-{index:02}.cachegrind"
                command = [args.valgrind, "--tool=cachegrind", "--cache-sim=yes",
                           "--instr-at-start=no", "--D1=32768,8,64", "--I1=32768,8,64",
                           "--LL=262144,8,64", f"--cachegrind-out-file={cg_path}", str(binary)]
                process = run(command)
                (case_dir / f"prefix-{index:02}.log").write_text(process.stderr)
                match = re.fullmatch(r"(0x[0-9a-fA-F]+)\s+\d+\s*", process.stdout)
                if not match:
                    raise RuntimeError(f"{name} prefix {index}: invalid kernel output: {process.stdout!r}")
                base = int(match.group(1), 16)
                if base % 4096:
                    raise RuntimeError(f"{name} prefix {index}: data buffer is not aligned")
                cg = kernel_counts(cg_path, event_lines)
                replay_input = "".join(f"{kind} {base + offset}\n" for kind, offset in prefix)
                (case_dir / f"prefix-{index:02}.accesses").write_text(replay_input)
                cs = tuple(map(int, run([str(replay_binary)], input=replay_input).stdout.split()))
                if cs != cg:
                    raise RuntimeError(f"{name} prefix {index}: CacheScope={cs}, Cachegrind={cg}")
                if index == len(accesses) and cs != GOLDEN[name]:
                    raise RuntimeError(f"{name}: {cs} != hand-calculated {GOLDEN[name]}")
                results[name].append({"prefix": index, "base": hex(base),
                                      "reads": cs[0], "read_misses": cs[1],
                                      "writes": cs[2], "write_misses": cs[3]})
    report = {"valgrind_version": version, "cc": run([args.cc, "--version"]).stdout.splitlines()[0],
              "cxx": run([args.cxx, "--version"]).stdout.splitlines()[0],
              "replay_compile_command": compile_command,
              "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in sources + [ROOT / "scripts/compare_cachegrind.py"]},
              "config": {"D1_size": 32768, "line_size": 64, "ways": 8, "policy": "LRU"},
              "cases": results}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Matched {sum(map(len, results.values()))} Cachegrind prefixes; results: {output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"comparison failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout or "", error.stderr or "", file=sys.stderr)
        sys.exit(1)
