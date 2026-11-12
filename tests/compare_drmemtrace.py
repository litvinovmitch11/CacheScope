#!/usr/bin/env python3
"""Compare semantic MLIR events with an independent native drmemtrace trace."""
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
from trace_demo import Variant, compile_variant  # noqa: E402
from cache_pipeline_e2e import validate_small  # noqa: E402


def check(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(command: list[object], output: Path | None = None) -> subprocess.CompletedProcess[str]:
    args = [str(arg) for arg in command]
    print("+", shlex.join(args), flush=True)
    result = subprocess.run(args, check=True, text=True, capture_output=True)
    if output is not None:
        output.write_text(result.stdout + result.stderr, encoding="utf-8")
    return result


def make_kernel(payload: Path, output: Path) -> None:
    original = payload.read_text(encoding="utf-8")
    declaration = "func.func private @matmul_kernel(%a: memref<8x8xf32>) {"
    main_marker = "  func.func @main()"
    check(original.count(declaration) == 1 and original.count(main_marker) == 1,
          "fixture shape changed; cannot derive uninstrumented kernel safely")
    kernel = original[:original.index(main_marker)] + "}\n"
    kernel = kernel.replace(declaration,
                            "func.func @matmul_kernel(%a: memref<8x8xf32>) "
                            "attributes {llvm.emit_c_interface} {")
    output.write_text(kernel, encoding="utf-8")


def symbol_range(binary: Path, output: Path) -> tuple[int, int]:
    result = run(["nm", "-S", "--defined-only", binary], output)
    matches = re.findall(r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+[Tt]\s+matmul_kernel$",
                         result.stdout, re.MULTILINE)
    check(len(matches) == 1, "expected exactly one native kernel symbol with size")
    address, size = (int(value, 16) for value in matches[0])
    check(size > 0, "native kernel has zero size")
    return address, address + size


def read_machine_trace(view: Path, base: int, size: int,
                       pc_start: int, pc_end: int) -> list[dict]:
    fetch = re.compile(r"\b(W\d+\.T\d+)\s+ifetch\s+\d+\s+byte\(s\)\s+@\s+(0x[0-9a-fA-F]+)")
    access = re.compile(r"\b(W\d+\.T\d+)\s+(read|write|prefetch)\s+(\d+)\s+byte\(s\)\s+@\s+"
                        r"(0x[0-9a-fA-F]+)\s+by PC\s+(0x[0-9a-fA-F]+)")
    events = []
    entries = []
    exits = set()
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
                if pc_start <= pc < pc_end and addr < base + size and addr + length > base:
                    check(kind in ("read", "write"), "unexpected prefetch in buffer")
                    check(base <= addr and addr + length <= base + size,
                          "native access crosses the buffer boundary")
                    events.append({"thread": thread, "kind": kind, "byte_offset": addr - base,
                                   "size_bytes": length, "pc": hex(pc)})
            match = re.search(r"<thread (W\d+\.T\d+) exited>", line)
            if match:
                exits.add(match.group(1))
            if "View tool results:" in line:
                complete = True
    check(complete, "drmemtrace view is missing its final results")
    check(len(entries) == 1, f"expected one native kernel invocation, got {len(entries)}")
    check(entries[0] in exits, "native kernel thread did not finish in the trace")
    check(all(event["thread"] == entries[0] for event in events),
          "buffer accesses came from another thread")
    return events


def execute(args: argparse.Namespace, output: Path) -> None:
    payload = ROOT / "tests/fixtures/cache_pipeline.mlir"
    driver = ROOT / "tests/fixtures/drmemtrace_driver.c"
    llvm = args.mlir_bin
    dr = args.dynamorio
    semantic = compile_variant(Variant("semantic", None), payload=payload,
                               output_root=output, tool_dir=llvm,
                               plugin=args.plugin, runtime=args.runtime)
    validate_small(semantic)

    native = output / "native"
    native.mkdir()
    kernel_mlir = native / "kernel.mlir"
    llvm_mlir = native / "kernel-llvm.mlir"
    llvm_ir = native / "kernel.ll"
    binary = native / "kernel"
    make_kernel(payload, kernel_mlir)
    run([llvm / "mlir-opt", kernel_mlir,
         "--pass-pipeline=builtin.module(canonicalize,cse,expand-strided-metadata,"
         "lower-affine,convert-scf-to-cf,convert-to-llvm,reconcile-unrealized-casts)",
         "-o", llvm_mlir])
    run([llvm / "mlir-translate", "--mlir-to-llvmir", llvm_mlir, "-o", llvm_ir])
    compile_command = [llvm / "clang", "-O0", "-g", "-fno-pie", "-no-pie", "-fno-stack-protector",
                       llvm_ir, driver, "-o", binary]
    run(compile_command, native / "compile.log")
    start, end = symbol_range(binary, native / "symbols.txt")

    raw_dir = native / "raw"
    raw_dir.mkdir()
    drrun = dr / "bin64/drrun"
    launcher = dr / "tools/bin64/drmemtrace_launcher"
    trace_command = [drrun, "-t", "drmemtrace", "-offline", "-outdir", raw_dir, "--", binary]
    traced = run(trace_command, native / "execution.log")
    match = re.fullmatch(r"base=(0x[0-9a-fA-F]+) size=(\d+) result=16\s*", traced.stdout)
    check(match is not None, f"native kernel returned unexpected output: {traced.stdout!r}")
    base, size = int(match.group(1), 16), int(match.group(2))
    check(size == 256 and base % 64 == 0, "unexpected buffer size or alignment")
    traces = sorted(raw_dir.glob("drmemtrace.*.dir"))
    check(len(traces) == 1, f"expected one drmemtrace directory, found {len(traces)}")
    view = native / "view.txt"
    view_command = [launcher, "-indir", traces[0], "-tool", "view"]
    print("+", shlex.join([str(arg) for arg in view_command]), flush=True)
    with view.open("w", encoding="utf-8") as stream:
        subprocess.run([str(arg) for arg in view_command], check=True, text=True,
                       stdout=stream, stderr=subprocess.STDOUT)
    observed = read_machine_trace(view, base, size, start, end)
    expected = [{"kind": event["kind"], "byte_offset": event["byte_offset"],
                 "size_bytes": event["size_bytes"]} for event in semantic["events"]]
    normalized = [{key: event[key] for key in ("kind", "byte_offset", "size_bytes")}
                  for event in observed]
    (native / "normalized-events.json").write_text(json.dumps(observed, indent=2) + "\n")
    if normalized != expected:
        mismatch = next((index for index, pair in enumerate(zip(normalized, expected))
                         if pair[0] != pair[1]), min(len(normalized), len(expected)))
        raise RuntimeError(f"semantic/machine trace mismatch at event {mismatch}: "
                           f"semantic={expected[mismatch:mismatch+1]}, "
                           f"machine={normalized[mismatch:mismatch+1]}, "
                           f"counts={len(expected)}/{len(normalized)}")
    report = {"semantic_events": expected, "machine_events": observed,
              "buffer_base": hex(base), "buffer_size": size,
              "kernel_pc_start": hex(start), "kernel_pc_end": hex(end),
              "native_compiler": run([llvm / "clang", "--version"]).stdout.splitlines()[0],
              "dynamorio_version": run([drrun, "-version"]).stdout.strip(),
              "drrun_sha256": hashlib.sha256(drrun.read_bytes()).hexdigest(),
              "compile_command": [str(arg) for arg in compile_command],
              "trace_command": [str(arg) for arg in trace_command],
              "view_command": [str(arg) for arg in view_command]}
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Matched {len(expected)} semantic and native buffer accesses: {output}")


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
        with tempfile.TemporaryDirectory(prefix="cachescope-drmemtrace-") as directory:
            execute(args, Path(directory))


if __name__ == "__main__":
    main()
