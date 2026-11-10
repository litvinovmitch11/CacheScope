#!/usr/bin/env python3
"""PyTorch -> MLIR transforms -> native execution -> CacheScope traces."""

from __future__ import annotations

import ctypes
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
from torch_mlir.fx import export_and_import
from torch_mlir.runtime import get_ranked_memref_descriptor


MATRIX_SIZE = 16


class Matmul(torch.nn.Module):
    def forward(self, left: torch.Tensor, right: torch.Tensor) -> torch.Tensor:
        return torch.mm(left, right)


@dataclass(frozen=True)
class Strategy:
    name: str
    transform: Path | None


ROOT = Path(__file__).resolve().parents[2]


def pinned_llvm_root() -> Path:
    version_file = ROOT / "cmake/LLVMVersion.cmake"
    prefix = 'set(CACHESCOPE_LLVM_MAJOR_VERSION "'
    for line in version_file.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix) and line.endswith('")'):
            return Path(f"/usr/lib/llvm-{line[len(prefix) : -2]}")
    raise RuntimeError(f"Cannot read LLVM major version from {version_file}")


LLVM_ROOT = Path(os.environ.get("LLVM_ROOT", pinned_llvm_root()))
BUILD = Path(os.environ.get("BUILD_DIR", ROOT / "build"))
TOOLS = LLVM_ROOT / "bin"
OUTPUT = Path(os.environ.get("CACHESCOPE_OUTPUT_DIR", ROOT / "output")) / "torch-demo"
PLUGIN = BUILD / "lib" / "CacheScopePassPlugin.so"
RUNTIME = BUILD / "compiler/runtime/libcachescope_trace_runtime.so"

STRATEGIES = (
    Strategy("baseline", None),
    Strategy("tile4", ROOT / "examples/transforms/matmul-tile4.transform.mlir"),
    Strategy(
        "tile4-ikj", ROOT / "examples/transforms/matmul-tile4-ikj.transform.mlir"
    ),
)


def tool(name: str) -> Path:
    path = TOOLS / name
    if not path.is_file():
        raise SystemExit(f"Не найден {path}. Установите LLVM/MLIR dependencies.")
    return path


def run(*args: object, stdout: Path | None = None) -> None:
    command = [str(arg) for arg in args]
    print("+", " ".join(command), flush=True)
    if stdout is None:
        subprocess.run(command, cwd=ROOT, check=True)
        return
    with stdout.open("w", encoding="utf-8") as output:
        subprocess.run(command, cwd=ROOT, check=True, stdout=output)


def configure_and_build_plugin() -> None:
    run(
        "cmake",
        "-S",
        ROOT,
        "-B",
        BUILD,
        "-G",
        "Ninja",
        f"-DMLIR_DIR={LLVM_ROOT / 'lib/cmake/mlir'}",
        f"-DCACHESCOPE_CLANG_TOOLS_DIR={LLVM_ROOT / 'bin'}",
        f"-DCMAKE_C_COMPILER={LLVM_ROOT / 'bin/clang'}",
        f"-DCMAKE_CXX_COMPILER={LLVM_ROOT / 'bin/clang++'}",
        "-DCMAKE_BUILD_TYPE=Release",
    )
    run("cmake", "--build", BUILD)


def export_pytorch(left: torch.Tensor, right: torch.Tensor) -> Path:
    module = export_and_import(
        Matmul().eval(),
        left,
        right,
        output_type="linalg-on-tensors",
        func_name="forward",
    )
    payload = OUTPUT / "00-pytorch-linalg.mlir"
    payload.write_text(str(module), encoding="utf-8")
    if "linalg.matmul" not in str(module):
        raise RuntimeError("torch-mlir export did not produce linalg.matmul")
    return payload


def add_native_entry(module_path: Path, output_path: Path) -> None:
    """Add a fixed-shape C ABI entry point after tensor bufferization."""
    text = module_path.read_text(encoding="utf-8")
    closing_brace = text.rfind("}")
    if closing_brace < 0:
        raise RuntimeError(f"cannot find module closing brace in {module_path}")

    memref = "memref<16x16xf32, strided<[?, ?], offset: ?>>"
    wrapper = f"""
  func.func @cachescope_entry(%left: {memref}, %right: {memref},
                              %output: {memref})
      attributes {{llvm.emit_c_interface}} {{
    %result = func.call @forward(%left, %right)
      : ({memref}, {memref}) -> memref<16x16xf32>
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c16 = arith.constant 16 : index
    scf.for %i = %c0 to %c16 step %c1 {{
      scf.for %j = %c0 to %c16 step %c1 {{
        %value = memref.load %result[%i, %j] : memref<16x16xf32>
        memref.store %value, %output[%i, %j] : {memref}
      }}
    }}
    memref.dealloc %result : memref<16x16xf32>
    return
  }}
"""
    output_path.write_text(
        text[:closing_brace] + wrapper + text[closing_brace:], encoding="utf-8"
    )


def compile_strategy(strategy: Strategy, payload: Path) -> dict[str, Path]:
    directory = OUTPUT / strategy.name
    directory.mkdir(parents=True, exist_ok=True)
    transformed = directory / "01-transformed.mlir"
    bufferized = directory / "02-bufferized.mlir"
    wrapped = directory / "03-native-entry.mlir"
    loops = directory / "04-loops.mlir"
    instrumented = directory / "05-instrumented.mlir"
    llvm_mlir = directory / "06-llvm.mlir"
    llvm_ir = directory / "07-module.ll"
    object_file = directory / "08-module.o"
    library = directory / f"libcachescope-{strategy.name}.so"
    trace = directory / "trace.json"

    if strategy.transform is None:
        run(tool("mlir-opt"), payload, "-o", transformed)
    else:
        run(
            tool("mlir-transform-opt"),
            payload,
            f"--transform={strategy.transform}",
            stdout=transformed,
        )

    run(
        tool("mlir-opt"),
        transformed,
        "--one-shot-bufferize=bufferize-function-boundaries",
        "-o",
        bufferized,
    )
    add_native_entry(bufferized, wrapped)
    run(
        tool("mlir-opt"),
        wrapped,
        "--convert-linalg-to-loops",
        "--canonicalize",
        "--cse",
        "-o",
        loops,
    )
    run(
        tool("mlir-opt"),
        loops,
        f"--load-pass-plugin={PLUGIN}",
        "--pass-pipeline=builtin.module(cachescope-instrument-memory{target-func=forward},canonicalize,cse)",
        "--mlir-print-debuginfo",
        "-o",
        instrumented,
    )
    run(
        tool("mlir-opt"),
        instrumented,
        f"--load-pass-plugin={PLUGIN}",
        "--pass-pipeline=builtin.module(expand-strided-metadata,lower-affine,convert-scf-to-cf,convert-to-llvm,reconcile-unrealized-casts,cachescope-instrument-llvm-fallback{target-func=forward})",
        "-o",
        llvm_mlir,
    )
    run(tool("mlir-translate"), llvm_mlir, "--mlir-to-llvmir", "-o", llvm_ir)
    run(
        tool("llc"),
        "-filetype=obj",
        "-relocation-model=pic",
        llvm_ir,
        "-o",
        object_file,
    )
    run(
        shutil.which("c++") or "c++",
        "-shared",
        object_file,
        RUNTIME,
        f"-Wl,-rpath,{RUNTIME.parent}",
        "-o",
        library,
    )
    return {"library": library, "trace": trace}


def execute_strategy(
    strategy: Strategy,
    artifacts: dict[str, Path],
    left: torch.Tensor,
    right: torch.Tensor,
) -> tuple[torch.Tensor, dict]:
    left_array = np.ascontiguousarray(left.detach().cpu().numpy(), dtype=np.float32)
    right_array = np.ascontiguousarray(right.detach().cpu().numpy(), dtype=np.float32)
    output_array = np.empty((MATRIX_SIZE, MATRIX_SIZE), dtype=np.float32)

    left_descriptor = get_ranked_memref_descriptor(left_array)
    right_descriptor = get_ranked_memref_descriptor(right_array)
    output_descriptor = get_ranked_memref_descriptor(output_array)

    library = ctypes.CDLL(str(artifacts["library"]), mode=os.RTLD_LOCAL)
    entry = library._mlir_ciface_cachescope_entry
    entry.argtypes = [
        ctypes.POINTER(type(left_descriptor)),
        ctypes.POINTER(type(right_descriptor)),
        ctypes.POINTER(type(output_descriptor)),
    ]
    entry.restype = None

    reset = library.__cachescope_trace_reset_v1
    reset.argtypes = []
    reset.restype = None
    flush = library.__cachescope_trace_flush_v1
    flush.argtypes = []
    flush.restype = None

    os.environ["CACHESCOPE_TRACE_FILE"] = str(artifacts["trace"])
    os.environ["CACHESCOPE_VARIANT"] = strategy.name
    os.environ["CACHESCOPE_PIPELINE_FINGERPRINT"] = (
        f"torch-mlir-matmul-{strategy.name}-v1"
    )
    os.environ.setdefault("CACHESCOPE_TRACE_LIMIT", "1000000")
    reset()
    entry(
        ctypes.byref(left_descriptor),
        ctypes.byref(right_descriptor),
        ctypes.byref(output_descriptor),
    )
    flush()

    with artifacts["trace"].open(encoding="utf-8") as trace_file:
        trace = json.load(trace_file)
    return torch.from_numpy(output_array.copy()), trace


def event_key(event: dict) -> tuple:
    return (
        event["access_id"],
        event["memref_id"],
        event["byte_offset"],
        event["kind"],
    )


def access_key(access: dict) -> tuple:
    return (
        access["access_id"],
        access["memref_id"],
        access["kind"],
        access["address_kind"],
    )


def compare_traces(traces: dict[str, dict]) -> dict:
    comparisons: dict[str, dict] = {}
    pairs = (
        ("baseline", "tile4"),
        ("baseline", "tile4-ikj"),
        ("tile4", "tile4-ikj"),
    )
    for left_name, right_name in pairs:
        left_trace = traces[left_name]
        right_trace = traces[right_name]
        pair_name = f"{left_name}_vs_{right_name}"
        if [access_key(access) for access in left_trace["accesses"]] != [
            access_key(access) for access in right_trace["accesses"]
        ]:
            raise RuntimeError(f"{pair_name}: static access tables differ")
        if left_trace["total_events"] != right_trace["total_events"]:
            raise RuntimeError(f"{pair_name}: dynamic event count differs")

        first_difference = None
        for sequence, (left_event, right_event) in enumerate(
            zip(left_trace["events"], right_trace["events"], strict=True)
        ):
            if event_key(left_event) != event_key(right_event):
                first_difference = {
                    "sequence": sequence,
                    left_name: event_key(left_event),
                    right_name: event_key(right_event),
                }
                break
        if first_difference is None:
            raise RuntimeError(f"{pair_name}: dynamic traces are identical")
        comparisons[pair_name] = first_difference
    return comparisons


def main() -> int:
    sys.path.insert(0, str(ROOT / "scripts"))
    from validate_trace import validate_trace

    for dependency in ("mlir-opt", "mlir-transform-opt", "mlir-translate", "llc"):
        tool(dependency)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    configure_and_build_plugin()

    torch.manual_seed(7)
    left = torch.randn(MATRIX_SIZE, MATRIX_SIZE, dtype=torch.float32)
    right = torch.randn(MATRIX_SIZE, MATRIX_SIZE, dtype=torch.float32)
    expected = Matmul().eval()(left, right)
    input_archive = OUTPUT / "inputs.npz"
    np.savez(
        input_archive,
        left=left.detach().cpu().numpy(),
        right=right.detach().cpu().numpy(),
    )
    input_manifest = {
        "schema": "cachescope.input.v1",
        "model": "Matmul.forward",
        "seed": 7,
        "dtype": "float32",
        "inputs": [
            {"name": "left", "shape": [MATRIX_SIZE, MATRIX_SIZE]},
            {"name": "right", "shape": [MATRIX_SIZE, MATRIX_SIZE]},
        ],
        "archive": input_archive.name,
    }
    (OUTPUT / "input.json").write_text(
        json.dumps(input_manifest, indent=2) + "\n", encoding="utf-8"
    )
    payload = export_pytorch(left, right)
    traces: dict[str, dict] = {}
    for strategy in STRATEGIES:
        artifacts = compile_strategy(strategy, payload)
        actual, trace = execute_strategy(strategy, artifacts, left, right)
        validate_trace(trace)
        torch.testing.assert_close(actual, expected, rtol=1e-5, atol=1e-5)
        if not trace.get("valid") or not trace.get("complete"):
            raise RuntimeError(f"{strategy.name}: trace is invalid or incomplete")
        traces[strategy.name] = trace
        fallback_events = sum(
            event.get("address_kind") == "process_address"
            for event in trace["events"]
        )
        print(
            f"{strategy.name}: tensor OK, "
            f"events={trace['total_events']}, accesses={len(trace['accesses'])}, "
            f"fallback={fallback_events}"
        )

    comparisons = compare_traces(traces)
    report = {
        "input": "input.json",
        "variants": {
            name: {
                "total_events": trace["total_events"],
                "recorded_events": trace["recorded_events"],
                "valid": trace["valid"],
                "complete": trace["complete"],
                "semantic_events": sum(
                    event.get("address_kind") == "memref_offset"
                    for event in trace["events"]
                ),
                "fallback_events": sum(
                    event.get("address_kind") == "process_address"
                    for event in trace["events"]
                ),
                "trace": f"{name}/trace.json",
            }
            for name, trace in traces.items()
        },
        "first_dynamic_differences": comparisons,
    }
    report_path = OUTPUT / "comparison.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print("\nEnd-to-end demo OK")
    for pair_name, difference in comparisons.items():
        print(
            f"{pair_name}: first trace difference at event "
            f"{difference['sequence']}"
        )
    print(f"Artifacts: {OUTPUT}")
    print(f"Summary:   {report_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"Pipeline command failed with exit code {error.returncode}") from error
