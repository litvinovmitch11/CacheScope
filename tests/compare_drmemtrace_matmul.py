#!/usr/bin/env python3
"""Compare semantic and native matmul accesses for all compiler schedules."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples"))
from trace_demo import VARIANTS, compare_traces, compile_variant  # noqa: E402
from cache_pipeline_e2e import validate_matmul  # noqa: E402
from compare_drmemtrace import check, run, symbol_range  # noqa: E402


def make_kernel(loops: Path, output: Path) -> None:
    text = loops.read_text(encoding="utf-8")
    marker = "  func.func @main()"
    check(text.count(marker) == 1, "expected one main in loop-stage MLIR")
    text = text[:text.index(marker)] + "}\n"
    text, count = re.subn(
        r"func.func private @matmul_kernel\(([^\n]+)\) \{",
        r"func.func @matmul_kernel(\1) attributes {llvm.emit_c_interface} {", text)
    check(count == 1, "expected one private matmul kernel in loop-stage MLIR")
    output.write_text(text, encoding="utf-8")


def read_machine_trace(view: Path, buffers: list[tuple[int, int]],
                       pc_start: int, pc_end: int) -> list[dict]:
    fetch = re.compile(r"\b(W\d+\.T\d+)\s+ifetch\s+\d+\s+byte\(s\)\s+@\s+(0x[0-9a-fA-F]+)")
    access = re.compile(r"\b(W\d+\.T\d+)\s+(read|write|prefetch)\s+(\d+)\s+byte\(s\)\s+@\s+"
                        r"(0x[0-9a-fA-F]+)\s+by PC\s+(0x[0-9a-fA-F]+)")
    entries = []
    exits = set()
    events = []
    complete = False
    with view.open(encoding="utf-8") as lines:
        for line in lines:
            match = fetch.search(line)
            if match and int(match.group(2), 16) == pc_start:
                entries.append(match.group(1))
            match = access.search(line)
            if match:
                thread, kind, length, addr, pc = match.groups()
                length, addr, pc = int(length), int(addr, 16), int(pc, 16)
                if pc_start <= pc < pc_end:
                    overlaps = [(index, base, size) for index, (base, size) in enumerate(buffers)
                                if addr < base + size and addr + length > base]
                    check(len(overlaps) <= 1, "native access overlaps multiple buffers")
                    if overlaps:
                        index, base, size = overlaps[0]
                        check(kind in ("read", "write"), "unexpected buffer prefetch")
                        check(base <= addr and addr + length <= base + size,
                              "native access crosses a buffer boundary")
                        events.append({"thread": thread, "memref_id": index, "kind": kind,
                                       "byte_offset": addr - base, "size_bytes": length,
                                       "pc": hex(pc)})
            match = re.search(r"<thread (W\d+\.T\d+) exited>", line)
            if match:
                exits.add(match.group(1))
            if "View tool results:" in line:
                complete = True
    check(complete, "drmemtrace view is missing its final results")
    check(len(entries) == 1, f"expected one kernel invocation, got {len(entries)}")
    check(entries[0] in exits, "kernel thread did not finish in the trace")
    check(all(event["thread"] == entries[0] for event in events),
          "buffer accesses came from another thread")
    return events


def run_native(args: argparse.Namespace, output: Path, name: str, semantic: dict) -> dict:
    native = output / name / "native"
    native.mkdir()
    kernel_mlir = native / "kernel.mlir"
    llvm_mlir = native / "kernel-llvm.mlir"
    llvm_ir = native / "kernel.ll"
    binary = native / "kernel"
    make_kernel(output / name / "02-loops.mlir", kernel_mlir)
    run([args.mlir_bin / "mlir-opt", kernel_mlir,
         "--pass-pipeline=builtin.module(expand-strided-metadata,lower-affine,"
         "convert-scf-to-cf,convert-to-llvm,reconcile-unrealized-casts)", "-o", llvm_mlir])
    run([args.mlir_bin / "mlir-translate", "--mlir-to-llvmir", llvm_mlir, "-o", llvm_ir])
    compile_command = [args.mlir_bin / "clang", "-O0", "-g", "-fno-pie", "-no-pie",
                       "-fno-stack-protector", llvm_ir,
                       ROOT / "tests/fixtures/drmemtrace_matmul_driver.c", "-o", binary]
    run(compile_command, native / "compile.log")
    start, end = symbol_range(binary, native / "symbols.txt")
    raw_dir = native / "raw"
    raw_dir.mkdir()
    drrun = args.dynamorio / "bin64/drrun"
    launcher = args.dynamorio / "tools/bin64/drmemtrace_launcher"
    trace_command = [drrun, "-t", "drmemtrace", "-offline", "-outdir", raw_dir, "--", binary]
    traced = run(trace_command, native / "execution.log")
    match = re.fullmatch(r"a=(0x[0-9a-fA-F]+) b=(0x[0-9a-fA-F]+) "
                         r"c=(0x[0-9a-fA-F]+) size=(\d+) result=16\s*", traced.stdout)
    check(match is not None, f"{name}: unexpected native output: {traced.stdout!r}")
    bases = [int(match.group(index), 16) for index in range(1, 4)]
    size = int(match.group(4))
    check(size == 1024 and all(base % 64 == 0 for base in bases),
          f"{name}: unexpected buffer size or alignment")
    buffers = [(base, size) for base in bases]
    for i, (base, length) in enumerate(buffers):
        for other, other_length in buffers[i + 1:]:
            check(base + length <= other or other + other_length <= base,
                  f"{name}: overlapping native buffers")
    traces = sorted(raw_dir.glob("drmemtrace.*.dir"))
    check(len(traces) == 1, f"{name}: expected one drmemtrace directory, got {len(traces)}")
    view = native / "view.txt"
    view_command = [launcher, "-indir", traces[0], "-tool", "view"]
    print("+", shlex.join([str(arg) for arg in view_command]), flush=True)
    with view.open("w", encoding="utf-8") as stream:
        subprocess.run([str(arg) for arg in view_command], check=True, text=True,
                       stdout=stream, stderr=subprocess.STDOUT)
    observed = read_machine_trace(view, buffers, start, end)
    keys = ("memref_id", "kind", "byte_offset", "size_bytes")
    expected = [{key: event[key] for key in keys} for event in semantic["events"]]
    normalized = [{key: event[key] for key in keys} for event in observed]
    (native / "normalized-events.json").write_text(json.dumps(observed, indent=2) + "\n")
    if normalized != expected:
        mismatch = next((index for index, pair in enumerate(zip(normalized, expected))
                         if pair[0] != pair[1]), min(len(normalized), len(expected)))
        raise RuntimeError(f"{name}: semantic/machine mismatch at event {mismatch}: "
                           f"semantic={expected[mismatch:mismatch+1]}, "
                           f"machine={normalized[mismatch:mismatch+1]}, "
                           f"counts={len(expected)}/{len(normalized)}")
    result = {"variant": name, "events": len(expected),
              "buffers": [{"memref_id": i, "base": hex(base), "size": length}
                          for i, (base, length) in enumerate(buffers)],
              "kernel_pc_start": hex(start), "kernel_pc_end": hex(end),
              "compile_command": [str(arg) for arg in compile_command],
              "trace_command": [str(arg) for arg in trace_command],
              "view_command": [str(arg) for arg in view_command]}
    (native / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"{name}: matched {len(expected)} semantic and native accesses")
    return result


def execute(args: argparse.Namespace, output: Path) -> None:
    traces = {}
    for variant in VARIANTS:
        trace = compile_variant(variant, payload=ROOT / "examples/workloads/matmul.mlir",
                                output_root=output, tool_dir=args.mlir_bin,
                                plugin=args.plugin, runtime=args.runtime)
        validate_matmul(trace, variant.name)
        traces[variant.name] = trace
    compare_traces(traces)
    results = [run_native(args, output, variant.name, traces[variant.name])
               for variant in VARIANTS]
    drrun = args.dynamorio / "bin64/drrun"
    summary = {"variants": results,
               "native_compiler": run([args.mlir_bin / "clang", "--version"]).stdout.splitlines()[0],
               "dynamorio_version": run([drrun, "-version"]).stdout.strip(),
               "drrun_sha256": hashlib.sha256(drrun.read_bytes()).hexdigest()}
    (output / "comparison.json").write_text(json.dumps(summary, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("mlir-bin", "plugin", "runtime", "dynamorio"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    for name in ("mlir_bin", "plugin", "runtime", "dynamorio"):
        setattr(args, name, getattr(args, name).resolve())
    if args.output_dir:
        output = args.output_dir.resolve()
        output.mkdir(parents=True, exist_ok=False)
        execute(args, output)
    else:
        with tempfile.TemporaryDirectory(prefix="cachescope-drmemtrace-matmul-") as directory:
            execute(args, Path(directory))


if __name__ == "__main__":
    main()
