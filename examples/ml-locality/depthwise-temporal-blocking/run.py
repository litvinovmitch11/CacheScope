#!/usr/bin/env python3
"""Run the depthwise temporal-blocking experiment through CacheScope."""

from __future__ import annotations

import argparse
import copy
import ctypes
import hashlib
import importlib.metadata
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
from torch_mlir.fx import export_and_import
from torch_mlir.runtime import get_ranked_memref_descriptor

from model import (
    BATCH,
    CHANNELS,
    HEIGHT,
    KERNEL,
    OUTPUT_HEIGHT,
    OUTPUT_WIDTH,
    SEED,
    WIDTH,
    DepthwiseBlock,
    verify_references,
)


SCENARIO = Path(__file__).resolve().parent
ROOT = SCENARIO.parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from validate_trace import validate_trace  # noqa: E402


@dataclass(frozen=True)
class Variant:
    name: str
    transform: Path
    tile_size: int | None


DEFAULT_TILE_SIZES = (6, 10, 14)
DEFAULT_STEPPER_EVENTS = 2048

DEMO_CACHE = ("L1:16384:64:4:lru",)
REFERENCE_HIERARCHY = (
    "L1:32768:64:8:lru",
    "L2:262144:64:8:lru",
    "L3:2097152:64:16:lru",
)
CACHE_PROFILES = {
    "demo": DEMO_CACHE,
    "reference": REFERENCE_HIERARCHY,
}


def pinned_llvm_root() -> Path:
    version_file = ROOT / "cmake/LLVMVersion.cmake"
    match = re.search(
        r'^set\(CACHESCOPE_LLVM_MAJOR_VERSION "([0-9]+)"\)$',
        version_file.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if not match:
        raise RuntimeError(f"cannot read LLVM version from {version_file}")
    return Path(f"/usr/lib/llvm-{match.group(1)}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare layer-wise and temporal-blocked depthwise CNN execution."
        )
    )
    parser.add_argument(
        "--llvm-root",
        type=Path,
        default=Path(os.environ.get("LLVM_ROOT", pinned_llvm_root())),
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path(os.environ.get("BUILD_DIR", ROOT / "build")),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(os.environ.get("CACHESCOPE_OUTPUT_DIR", ROOT / "output"))
        / "ml-locality/depthwise-temporal-blocking",
    )
    parser.add_argument(
        "--skip-ui",
        action="store_true",
        help="Skip generation of the standalone HTML report.",
    )
    parser.add_argument(
        "--npm-ci",
        action="store_true",
        help="Reinstall locked UI dependencies before building the report.",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Reuse an already built plugin, trace runtime, and runner.",
    )
    parser.add_argument(
        "--tile-sizes",
        default=",".join(str(tile) for tile in DEFAULT_TILE_SIZES),
        help=(
            "Comma-separated positive spatial tile sizes. The baseline is "
            "always included; default: %(default)s."
        ),
    )
    parser.add_argument(
        "--keep-events",
        action="store_true",
        help=(
            "Keep batch simulation events.json files. They are not needed by "
            "the report or interactive stepper and are large for this workload."
        ),
    )
    parser.add_argument(
        "--stepper-events",
        type=int,
        default=DEFAULT_STEPPER_EVENTS,
        help=(
            "Number of events in the compact interactive stepper bundle; "
            "use 0 to disable it. Default: %(default)s."
        ),
    )
    parser.add_argument(
        "--progress-file",
        type=Path,
        help="Atomically publish machine-readable stage progress for an orchestrator.",
    )
    return parser.parse_args()


def publish_progress(
    args: argparse.Namespace,
    stage: str,
    message: str,
    *,
    current: int | None = None,
    total: int | None = None,
    variant: str | None = None,
) -> None:
    if args.progress_file is None:
        return
    destination = args.progress_file.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    payload: dict[str, object] = {"stage": stage, "message": message}
    if current is not None:
        payload["current"] = current
    if total is not None:
        payload["total"] = total
    if variant is not None:
        payload["variant"] = variant
    temporary = destination.with_name(destination.name + f".tmp-{os.getpid()}")
    temporary.write_text(json.dumps(payload) + "\n", encoding="utf-8")
    os.replace(temporary, destination)


def parse_tile_sizes(value: str) -> tuple[int, ...]:
    try:
        parsed = tuple(int(item.strip()) for item in value.split(","))
    except ValueError as error:
        raise RuntimeError("tile sizes must be comma-separated integers") from error
    if not parsed or any(tile <= 0 for tile in parsed):
        raise RuntimeError("tile sizes must contain positive integers")
    if len(set(parsed)) != len(parsed):
        raise RuntimeError("tile sizes must be unique")
    return parsed


def prepare_variants(output: Path, tile_sizes: tuple[int, ...]) -> tuple[Variant, ...]:
    template = require_file(
        SCENARIO / "transforms/temporal-blocked.transform.mlir.in",
        "temporal-blocked transform template",
    ).read_text(encoding="utf-8")
    generated = output / "transforms"
    generated.mkdir(parents=True, exist_ok=True)
    variants = [
        Variant(
            "baseline",
            SCENARIO / "transforms/layerwise.transform.mlir",
            None,
        )
    ]
    for tile in tile_sizes:
        transform = generated / f"temporal-blocked-{tile}.transform.mlir"
        transform.write_text(template.replace("@TILE@", str(tile)), encoding="utf-8")
        variants.append(Variant(f"tile-{tile}", transform, tile))
    return tuple(variants)


def run(
    command: list[object],
    *,
    cwd: Path = ROOT,
    env: dict[str, str] | None = None,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    rendered = [str(argument) for argument in command]
    print("+", shlex.join(rendered), flush=True)
    return subprocess.run(
        rendered,
        cwd=cwd,
        env=env,
        check=True,
        text=True,
        capture_output=capture,
    )


def require_file(path: Path, description: str) -> Path:
    if not path.is_file():
        raise RuntimeError(f"missing {description}: {path}")
    return path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return "sha256:" + digest.hexdigest()


def package_version(distribution: str) -> str:
    try:
        return importlib.metadata.version(distribution)
    except importlib.metadata.PackageNotFoundError:
        return "unknown"


def captured(command: list[object], *, cwd: Path = ROOT) -> str:
    completed = subprocess.run(
        [str(argument) for argument in command],
        cwd=cwd,
        check=True,
        text=True,
        capture_output=True,
    )
    return completed.stdout.strip()


def is_scenario_output(path: Path) -> bool:
    input_manifest = path / "input.json"
    if not input_manifest.is_file():
        return False
    try:
        value = json.loads(input_manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    return value.get("scenario") == "depthwise-temporal-blocking"


def validate_output_destination(path: Path) -> None:
    if not path.exists():
        return
    if not path.is_dir():
        raise RuntimeError(f"output path is not a directory: {path}")
    if any(path.iterdir()) and not is_scenario_output(path):
        raise RuntimeError(
            f"refusing to replace non-CacheScope scenario output: {path}"
        )


def publish_output(staging: Path, destination: Path) -> None:
    validate_output_destination(destination)
    backup = destination.with_name(
        f".{destination.name}.previous-{os.getpid()}"
    )
    if backup.exists():
        raise RuntimeError(f"temporary output backup already exists: {backup}")
    if destination.exists():
        os.replace(destination, backup)
    try:
        os.replace(staging, destination)
    except BaseException:
        if backup.exists() and not destination.exists():
            os.replace(backup, destination)
        raise
    shutil.rmtree(backup, ignore_errors=True)


def find_library(directory: Path, stem: str) -> Path:
    for suffix in (".so", ".dylib", ".dll"):
        candidate = directory / f"{stem}{suffix}"
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"missing library: {directory / (stem + '.so')}")


def configure_and_build(
    args: argparse.Namespace,
) -> tuple[Path, Path, Path]:
    def resolve_artifacts() -> tuple[Path, Path, Path]:
        return (
            find_library(args.build_dir / "lib", "CacheScopePassPlugin"),
            find_library(
                args.build_dir / "compiler/runtime",
                "libcachescope_trace_runtime",
            ),
            require_file(
                args.build_dir / "runner/cachescope-runner",
                "runner executable",
            ),
        )

    if args.skip_build:
        return resolve_artifacts()

    run(
        [
            "cmake",
            "-S",
            ROOT,
            "-B",
            args.build_dir,
            "-G",
            "Ninja",
            f"-DMLIR_DIR={args.llvm_root / 'lib/cmake/mlir'}",
            f"-DCACHESCOPE_CLANG_TOOLS_DIR={args.llvm_root / 'bin'}",
            f"-DCMAKE_C_COMPILER={args.llvm_root / 'bin/clang'}",
            f"-DCMAKE_CXX_COMPILER={args.llvm_root / 'bin/clang++'}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCACHESCOPE_BUILD_CACHE_MODEL=ON",
            "-DBUILD_TESTING=ON",
        ]
    )
    run(
        [
            "cmake",
            "--build",
            args.build_dir,
            "--target",
            "CacheScopePassPlugin",
            "cachescope_trace_runtime",
            "cachescope_runner_cli",
        ]
    )
    return resolve_artifacts()


def tool(args: argparse.Namespace, name: str) -> Path:
    return require_file(args.llvm_root / "bin" / name, name)


def write_inputs(
    output: Path,
    inputs: tuple[torch.Tensor, ...],
    tile_sizes: tuple[int, ...],
) -> None:
    names = ("input", "weight1", "bias1", "weight2", "bias2")
    arrays = {
        name: tensor.detach().cpu().numpy()
        for name, tensor in zip(names, inputs, strict=True)
    }
    np.savez(output / "inputs.npz", **arrays)
    manifest = {
        "schema": "cachescope.ml-locality-input.v1",
        "scenario": "depthwise-temporal-blocking",
        "seed": SEED,
        "dtype": "float32",
        "tile_sizes": [[tile, tile] for tile in tile_sizes],
        "inputs": [
            {"name": name, "shape": list(array.shape)}
            for name, array in arrays.items()
        ],
        "archive": "inputs.npz",
    }
    (output / "input.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )


def write_experiment_manifest(
    output: Path,
    variants: tuple[Variant, ...],
    args: argparse.Namespace,
    payload: Path,
    plugin: Path,
    runtime: Path,
    runner: Path,
) -> None:
    revision = captured(["git", "rev-parse", "HEAD"])
    dirty = bool(captured(["git", "status", "--porcelain"]))
    mlir_version = captured([tool(args, "mlir-opt"), "--version"]).splitlines()[0]
    manifest = {
        "schema": "cachescope.experiment.v1",
        "name": "depthwise-temporal-blocking",
        "frontend": {
            "kind": "torch",
            "module": "model.py:DepthwiseBlock",
            "inputs": "inputs.npz",
        },
        "reproducibility": {
            "seed": SEED,
            "repository_revision": revision,
            "repository_dirty": dirty,
            "versions": {
                "python": sys.version.split()[0],
                "torch": torch.__version__,
                "torch_mlir": package_version("torch-mlir"),
                "numpy": np.__version__,
                "mlir": mlir_version,
            },
            "fingerprints": {
                "model": sha256_file(SCENARIO / "model.py"),
                "inputs": sha256_file(output / "inputs.npz"),
                "torch_payload": sha256_file(payload),
                "compiler_plugin": sha256_file(plugin),
                "trace_runtime": sha256_file(runtime),
                "runner": sha256_file(runner),
            },
        },
        "variants": [
            {
                "name": variant.name,
                "tile_size": variant.tile_size,
                "transform": str(variant.transform.relative_to(output))
                if variant.transform.is_relative_to(output)
                else str(variant.transform.relative_to(ROOT)),
                "transform_fingerprint": sha256_file(variant.transform),
            }
            for variant in variants
        ],
        "cache_profiles": {
            name: list(levels) for name, levels in CACHE_PROFILES.items()
        },
        "validation": {
            "torch_reference": {"rtol": 1e-5, "atol": 1e-5},
            "compiled_variants": "bitwise",
            "require_complete_trace": True,
            "require_semantic_addresses": True,
            "require_matching_provenance": True,
        },
        "artifacts": {
            "summary": "summary.json",
            "results": "RESULTS.md",
            "report": None if args.skip_ui else "report.html",
            "stepper_demo": (
                "stepper-demo" if args.stepper_events > 0 else None
            ),
        },
    }
    (output / "experiment.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )


def export_torch_graph(
    output: Path, inputs: tuple[torch.Tensor, ...]
) -> Path:
    module = export_and_import(
        DepthwiseBlock().eval(),
        *inputs,
        output_type="linalg-on-tensors",
        func_name="forward",
    )
    text = str(module)
    required = ("linalg.depthwise_conv_2d_nchw_chw", "linalg.generic")
    if any(operation not in text for operation in required):
        raise RuntimeError("torch-mlir did not produce the expected convolution graph")
    if text.count("linalg.depthwise_conv_2d_nchw_chw") != 2:
        raise RuntimeError("expected exactly two depthwise convolutions")
    payload = output / "00-pytorch-linalg.mlir"
    payload.write_text(text, encoding="utf-8")
    return payload


def add_native_entry(module_path: Path, output_path: Path) -> None:
    text = module_path.read_text(encoding="utf-8")
    module_start = text.find("module ")
    located_end = re.search(
        r"^} loc\(#loc[0-9]+\)$", text[module_start:], re.MULTILINE
    )
    if located_end is not None:
        closing_brace = module_start + located_end.start()
    else:
        plain_end = text.find("\n}\n", module_start)
        closing_brace = plain_end + 1 if plain_end >= 0 else -1
    if closing_brace < 0:
        raise RuntimeError(f"cannot find module closing brace in {module_path}")

    activation = f"memref<{BATCH}x{CHANNELS}x{HEIGHT}x{WIDTH}xf32>"
    weight = f"memref<{CHANNELS}x1x{KERNEL}x{KERNEL}xf32>"
    bias = f"memref<{CHANNELS}xf32>"
    output = (
        f"memref<{BATCH}x{CHANNELS}x{OUTPUT_HEIGHT}x{OUTPUT_WIDTH}xf32>"
    )
    result = output
    wrapper = f"""
  func.func @cachescope_entry(
      %input: {activation}, %weight1: {weight}, %bias1: {bias},
      %weight2: {weight}, %bias2: {bias}, %output: {output})
      attributes {{llvm.emit_c_interface}} {{
    %result = func.call @forward(
      %input, %weight1, %bias1, %weight2, %bias2)
      : ({activation}, {weight}, {bias}, {weight}, {bias}) -> {result}
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c_channels = arith.constant {CHANNELS} : index
    %c_output_height = arith.constant {OUTPUT_HEIGHT} : index
    %c_output_width = arith.constant {OUTPUT_WIDTH} : index
    scf.for %channel = %c0 to %c_channels step %c1 {{
      scf.for %height = %c0 to %c_output_height step %c1 {{
        scf.for %width = %c0 to %c_output_width step %c1 {{
          %value = memref.load %result[%c0, %channel, %height, %width]
            : {result}
          memref.store %value, %output[%c0, %channel, %height, %width]
            : {output}
        }}
      }}
    }}
    memref.dealloc %result : {result}
    return
  }}
"""
    output_path.write_text(
        text[:closing_brace] + wrapper + text[closing_brace:], encoding="utf-8"
    )


def compile_variant(
    args: argparse.Namespace,
    variant: Variant,
    payload: Path,
    plugin: Path,
    runtime: Path,
) -> dict[str, Path]:
    directory = args.output_dir / variant.name
    directory.mkdir(parents=True, exist_ok=True)
    input_tagged = directory / ".00-input-tagged.mlir"
    transformed_raw = directory / ".01-transformed-raw.mlir"
    transformed_tagged = directory / ".01-transformed-tagged.mlir"
    bufferized_raw = directory / ".02-bufferized-raw.mlir"
    bufferized = directory / ".02-bufferized.mlir"
    wrapped = directory / ".02-wrapped.mlir"
    loops_tagged = directory / ".02-loops-tagged.mlir"
    instrumented_tagged = directory / ".03-instrumented-tagged.mlir"
    llvm_tagged = directory / ".04-llvm-tagged.mlir"
    executable = directory / "05-executable.mlir"
    llvm_ir = directory / "06-module.ll"
    object_file = directory / "07-module.o"
    library = directory / f"libcachescope-{variant.name}.so"
    trace = directory / "trace.json"
    payload_argument = Path(os.path.relpath(payload, directory))

    mlir_opt = tool(args, "mlir-opt")
    run(
        [
            mlir_opt,
            payload_argument,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "snapshot-op-locations{filename=input.mlir tag=cachescope.stage.input},"
            "cachescope-tag-stage{target-func=forward stage-id=input})",
            "--mlir-print-debuginfo",
            "-o",
            input_tagged.name,
        ],
        cwd=directory,
    )
    transformed_text = run(
        [
            tool(args, "mlir-transform-opt"),
            input_tagged.name,
            f"--transform={variant.transform}",
            "--mlir-print-debuginfo",
        ],
        cwd=directory,
        capture=True,
    ).stdout
    transformed_raw.write_text(transformed_text, encoding="utf-8")
    run(
        [
            mlir_opt,
            transformed_raw.name,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "canonicalize,cse,"
            "snapshot-op-locations{filename=01-transformed.mlir tag=cachescope.stage.after-transform},"
            "cachescope-tag-stage{target-func=forward stage-id=after-transform})",
            "--mlir-print-debuginfo",
            "-o",
            transformed_tagged.name,
        ],
        cwd=directory,
    )
    run(
        [
            mlir_opt,
            transformed_tagged.name,
            "--one-shot-bufferize=bufferize-function-boundaries "
            "function-boundary-type-conversion=identity-layout-map",
            "--canonicalize",
            "--cse",
            "--mlir-print-debuginfo",
            "-o",
            bufferized_raw.name,
        ],
        cwd=directory,
    )
    run(
        [
            mlir_opt,
            bufferized_raw.name,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "cachescope-assign-origins{target-func=forward})",
            "--mlir-print-debuginfo",
            "-o",
            bufferized.name,
        ],
        cwd=directory,
    )
    add_native_entry(bufferized, wrapped)
    run(
        [
            mlir_opt,
            wrapped.name,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "convert-linalg-to-loops,canonicalize,cse,"
            "snapshot-op-locations{filename=02-loops.mlir tag=cachescope.stage.after-linalg-to-loops},"
            "cachescope-tag-stage{target-func=forward stage-id=after-linalg-to-loops})",
            "--mlir-print-debuginfo",
            "-o",
            loops_tagged.name,
        ],
        cwd=directory,
    )
    run(
        [
            mlir_opt,
            loops_tagged.name,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "cachescope-instrument-memory{target-func=forward},canonicalize,cse,"
            "snapshot-op-locations{filename=03-instrumented.mlir tag=cachescope.stage.after-instrumentation},"
            "cachescope-tag-stage{target-func=forward stage-id=after-instrumentation})",
            "--mlir-print-debuginfo",
            "-o",
            instrumented_tagged.name,
        ],
        cwd=directory,
    )
    run(
        [
            mlir_opt,
            instrumented_tagged.name,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "expand-strided-metadata,lower-affine,convert-scf-to-cf,"
            "convert-to-llvm,reconcile-unrealized-casts,"
            "snapshot-op-locations{filename=04-llvm.mlir tag=cachescope.stage.after-llvm-lowering},"
            "cachescope-tag-stage{target-func=forward stage-id=after-llvm-lowering})",
            "--mlir-print-debuginfo",
            "-o",
            llvm_tagged.name,
        ],
        cwd=directory,
    )
    payload_fingerprint = "sha256:" + hashlib.sha256(
        (directory / "input.mlir").read_bytes()
    ).hexdigest()
    run(
        [
            mlir_opt,
            llvm_tagged.name,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "cachescope-instrument-llvm-fallback{"
            f"target-func=forward payload-fingerprint={payload_fingerprint}"
            "})",
            "-o",
            executable.name,
        ],
        cwd=directory,
    )
    run(
        [
            tool(args, "mlir-translate"),
            executable.name,
            "--mlir-to-llvmir",
            "-o",
            llvm_ir.name,
        ],
        cwd=directory,
    )
    run(
        [
            tool(args, "llc"),
            "-filetype=obj",
            "-relocation-model=pic",
            llvm_ir.name,
            "-o",
            object_file.name,
        ],
        cwd=directory,
    )
    run(
        [
            shutil.which("c++") or "c++",
            "-shared",
            object_file.name,
            runtime,
            f"-Wl,-rpath,{runtime.parent}",
            "-o",
            library.name,
        ],
        cwd=directory,
    )
    return {"directory": directory, "library": library, "trace": trace}


def execute_variant(
    variant: Variant,
    artifacts: dict[str, Path],
    inputs: tuple[torch.Tensor, ...],
) -> tuple[torch.Tensor, dict]:
    arrays = [
        np.ascontiguousarray(tensor.detach().cpu().numpy(), dtype=np.float32)
        for tensor in inputs
    ]
    output = np.empty(
        (BATCH, CHANNELS, OUTPUT_HEIGHT, OUTPUT_WIDTH), dtype=np.float32
    )
    descriptors = [get_ranked_memref_descriptor(array) for array in arrays]
    output_descriptor = get_ranked_memref_descriptor(output)

    library = ctypes.CDLL(str(artifacts["library"]), mode=os.RTLD_LOCAL)
    entry = library._mlir_ciface_cachescope_entry
    entry.argtypes = [
        *(ctypes.POINTER(type(descriptor)) for descriptor in descriptors),
        ctypes.POINTER(type(output_descriptor)),
    ]
    entry.restype = None
    reset = library.__cachescope_trace_reset_v1
    reset.argtypes = []
    reset.restype = None
    flush = library.__cachescope_trace_flush_v1
    flush.argtypes = []
    flush.restype = None

    os.environ.update(
        {
            "CACHESCOPE_TRACE_FILE": str(artifacts["trace"]),
            "CACHESCOPE_VARIANT": variant.name,
            "CACHESCOPE_PIPELINE_FINGERPRINT": (
                f"depthwise-temporal-blocking-{variant.name}-v1"
            ),
            "CACHESCOPE_TRACE_LIMIT": "2000000",
        }
    )
    reset()
    entry(
        *(ctypes.byref(descriptor) for descriptor in descriptors),
        ctypes.byref(output_descriptor),
    )
    flush()
    trace = json.loads(artifacts["trace"].read_text(encoding="utf-8"))
    validate_trace(trace, artifact_root=artifacts["directory"])
    if not trace["valid"] or not trace["complete"]:
        raise RuntimeError(f"{variant.name}: trace is invalid or incomplete")
    if any(
        event["address_kind"] != "memref_offset" for event in trace["events"]
    ):
        raise RuntimeError(f"{variant.name}: unexpected LLVM fallback event")
    return torch.from_numpy(output.copy()), trace


def replay(
    runner: Path,
    trace: Path,
    destination: Path,
    cache: tuple[str, ...],
    *,
    keep_events: bool,
) -> dict:
    command: list[object] = [
        runner,
        "--trace",
        trace,
        "--output-dir",
        destination,
    ]
    for level in cache:
        command.extend(["--cache-level", level])
    run(command)
    statistics = json.loads(
        (destination / "statistics.json").read_text(encoding="utf-8")
    )
    if not keep_events:
        (destination / "events.json").unlink(missing_ok=True)
    return statistics


def trace_metrics(trace: dict) -> dict[str, int]:
    events = trace["events"]
    return {
        "events": trace["total_events"],
        "reads": sum(event["kind"] == "read" for event in events),
        "writes": sum(event["kind"] == "write" for event in events),
        "requested_bytes": sum(event["size_bytes"] for event in events),
        "static_accesses": len(trace["accesses"]),
    }


def assert_comparable_provenance(traces: dict[str, dict]) -> None:
    baseline = traces["baseline"]
    baseline_map = baseline["source_map"]
    expected_stages = (
        "input",
        "after-transform",
        "after-linalg-to-loops",
        "after-instrumentation",
        "after-llvm-lowering",
    )

    for name, trace in traces.items():
        source_map = trace["source_map"]
        stage_ids = tuple(stage["stage_id"] for stage in source_map["stages"])
        if stage_ids != expected_stages:
            raise RuntimeError(f"{name}: incomplete source-map stage coverage")
        if (
            baseline_map["payload_fingerprint"]
            != source_map["payload_fingerprint"]
        ):
            raise RuntimeError(
                f"{name}: variant does not originate from the baseline payload"
            )
        if baseline_map["origins"] != source_map["origins"]:
            raise RuntimeError(
                f"{name}: fusion changed the scenario's high-level origins"
            )

    def signature(trace: dict) -> list[tuple]:
        result = []
        for access in trace["accesses"]:
            origin_ids = tuple(access.get("origin_ids", ()))
            stage_ids = tuple(
                location["stage_id"]
                for location in access.get("stage_locations", ())
            )
            if not origin_ids or stage_ids != expected_stages:
                raise RuntimeError(
                    f"access {access['access_id']} has incomplete provenance"
                )
            result.append(
                (
                    access["access_id"],
                    access["memref_id"],
                    access["kind"],
                    access["address_kind"],
                    origin_ids,
                    stage_ids,
                )
            )
        return result

    baseline_signature = signature(baseline)
    for name, trace in traces.items():
        if signature(trace) != baseline_signature:
            raise RuntimeError(f"{name}: fusion changed static access provenance")


def assert_comparable_simulations(
    simulations: dict[str, dict[str, dict]],
) -> None:
    for profile, variants in simulations.items():
        baseline = variants["baseline"]
        baseline_configs = [level["cache_config"] for level in baseline["levels"]]
        for name, statistics in variants.items():
            if baseline["buffers"] != statistics["buffers"]:
                raise RuntimeError(
                    f"{profile}/{name}: variants use different model buffer placement"
                )
            configs = [
                level["cache_config"] for level in statistics["levels"]
            ]
            if baseline_configs != configs:
                raise RuntimeError(
                    f"{profile}/{name}: variants use different cache configurations"
                )
            if baseline["hierarchy"] != statistics["hierarchy"]:
                raise RuntimeError(
                    f"{profile}/{name}: variants use different hierarchies"
                )
        expected_hierarchy = {
            "allocation_policy": "write_allocate",
            "inclusion_policy": "exclusive",
            "write_policy": "write_back",
        }
        if any(
            baseline["hierarchy"].get(key) != value
            for key, value in expected_hierarchy.items()
        ):
            raise RuntimeError(f"{profile}: unexpected hierarchy semantics")
        if any(
            policy != "exclusive"
            for policy in baseline["hierarchy"].get("inclusion_policies", ())
        ):
            raise RuntimeError(f"{profile}: expected an exclusive hierarchy")


def simulation_metrics(statistics: dict) -> dict:
    return {
        "hierarchy": statistics["hierarchy"],
        "levels": [
            {
                "name": level["name"],
                "cache_config": level["cache_config"],
                "hits": level["totals"]["hits"],
                "misses": level["totals"]["misses"],
                "hit_rate_percent": level["totals"]["hit_rate_percent"],
                "evictions": level["totals"]["evictions"],
            }
            for level in statistics["levels"]
        ],
        "memory_reads": statistics["memory"]["reads"],
        "memory_writebacks": statistics["memory"]["writebacks"],
    }


def node_is_supported() -> bool:
    if shutil.which("node") is None or shutil.which("npm") is None:
        return False
    version = run(["node", "--version"], capture=True).stdout.strip()
    match = re.fullmatch(r"v([0-9]+)\.([0-9]+)\.[0-9]+", version)
    if not match:
        return False
    major, minor = (int(value) for value in match.groups())
    return (major == 20 and minor >= 19) or (major == 22 and minor >= 12) or major > 22


def build_report_bundle(
    args: argparse.Namespace,
    variants: tuple[Variant, ...],
) -> Path:
    bundle = args.output_dir / "report-input"
    if bundle.exists():
        shutil.rmtree(bundle)
    (bundle / "simulation").mkdir(parents=True)
    for variant in variants:
        variant_bundle = bundle / variant.name
        variant_bundle.mkdir()
        os.link(
            args.output_dir / variant.name / "trace.json",
            variant_bundle / "trace.json",
        )
        for profile in CACHE_PROFILES:
            simulation = f"{profile}--{variant.name}"
            shutil.copytree(
                args.output_dir / "simulation" / simulation,
                bundle / "simulation" / simulation,
            )
    if not node_is_supported():
        raise RuntimeError("UI requires Node.js ^20.19.0 or >=22.12.0")
    ui = ROOT / "ui"
    if args.npm_ci or not (ui / "node_modules/.bin/vite").is_file():
        run(["npm", "ci"], cwd=ui)
    report = args.output_dir / "report.html"
    try:
        run(
            ["npm", "run", "report", "--", "--input", bundle, "--output", report],
            cwd=ui,
        )
    finally:
        shutil.rmtree(bundle, ignore_errors=True)
    return require_file(report, "HTML report")


def build_stepper_bundle(
    args: argparse.Namespace,
    variants: tuple[Variant, ...],
    traces: dict[str, dict],
) -> dict | None:
    if args.stepper_events == 0:
        return None
    if args.stepper_events < 0:
        raise RuntimeError("stepper event count must be non-negative")

    preferred = next(
        (variant for variant in variants if variant.tile_size == 10),
        next(variant for variant in variants if variant.tile_size is not None),
    )
    source = traces[preferred.name]
    event_count = min(args.stepper_events, len(source["events"]))
    preview = copy.deepcopy(source)
    preview["events"] = preview["events"][:event_count]
    preview["total_events"] = event_count
    preview["recorded_events"] = event_count
    preview["complete"] = True
    preview["truncated"] = False
    preview["producer"]["pipeline_fingerprint"] += (
        f"-stepper-prefix-{event_count}"
    )
    preview["producer"]["derived_from_variant"] = preferred.name
    preview["producer"]["source_total_events"] = source["total_events"]

    bundle = args.output_dir / "stepper-demo"
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir()
    trace_path = bundle / "trace.json"
    trace_path.write_text(json.dumps(preview) + "\n", encoding="utf-8")
    for stage in preview["source_map"]["stages"]:
        stage_path = Path(stage["path"])
        source_path = args.output_dir / preferred.name / stage_path
        destination = bundle / stage_path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_path, destination)
    validate_trace(preview, artifact_root=bundle)

    metadata = {
        "variant": preferred.name,
        "events": event_count,
        "source_total_events": source["total_events"],
        "trace": "stepper-demo/trace.json",
        "purpose": "interactive UI preview; excluded from experiment metrics",
    }
    (bundle / "README.md").write_text(
        "\n".join(
            [
                "# Interactive stepper demo bundle",
                "",
                f"Компактный prefix первых {event_count} событий варианта "
                f"`{preferred.name}`. Он предназначен только для живой "
                "пошаговой демонстрации и не участвует в расчёте метрик.",
                "",
                "В Docker UI откройте `/?view=stepper`, выберите все файлы "
                "этой директории и создайте сессию. Полный trace находится в "
                f"`../{preferred.name}/trace.json`.",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return metadata


def write_demo_guide(
    args: argparse.Namespace,
    stepper_demo: dict | None,
    report: Path | None,
) -> None:
    lines = [
        "# CacheScope presentation demo",
        "",
        "## 1. Сравнительный результат",
        "",
        "Откройте `RESULTS.md`, затем `report.html` и покажите tile-size sweep "
        "на одинаковых inputs и cache profiles.",
        "",
        "Ключевой тезис: CacheScope сравнивает low-level schedules одной "
        "Torch-модели и объясняет компромисс между locality и дополнительной "
        "работой, а cache simulator служит измерительным механизмом.",
    ]
    if report is None:
        lines.extend(["", "Standalone report отключён флагом `--skip-ui`."])
    if stepper_demo is not None:
        lines.extend(
            [
                "",
                "## 2. Интерактивное объяснение",
                "",
                "1. Из корня репозитория выполните `docker compose up --build`.",
                "2. Откройте `http://localhost:9055/?view=stepper`.",
                "3. Выберите все файлы из `stepper-demo/`.",
                "4. Создайте сессию и покажите event, MLIR source mapping, "
                "sets/ways, heatmap и hit/miss timeline.",
                "",
                f"Stepper bundle содержит {stepper_demo['events']} событий "
                f"варианта `{stepper_demo['variant']}` и весит существенно "
                "меньше полного trace. Он не используется для итоговых метрик.",
            ]
        )
    lines.extend(
        [
            "",
            "## 3. Доказательства корректности",
            "",
            "При вопросах жюри покажите `experiment.json`: там зафиксированы "
            "версии, fingerprints, transforms, cache profiles и validation "
            "policy. `summary.json` содержит машинно-читаемые результаты.",
            "",
        ]
    )
    (args.output_dir / "DEMO.md").write_text(
        "\n".join(lines), encoding="utf-8"
    )


def publish_results(
    args: argparse.Namespace,
    variants: tuple[Variant, ...],
    traces: dict[str, dict],
    simulations: dict[str, dict[str, dict]],
    report: Path | None,
    stepper_demo: dict | None,
) -> None:
    summary = {
        "schema": "cachescope.ml-locality-experiment.v1",
        "scenario": "depthwise-temporal-blocking",
        "article": {
            "title": "Optimal Temporal Blocking for Stencil Computation",
            "doi": "10.1016/j.procs.2015.05.315",
            "url": (
                "https://www.sciencedirect.com/science/article/"
                "pii/S1877050915011230"
            ),
        },
        "operation": "DWConv3x3(ReLU(DWConv3x3(input)))",
        "input_shape": [BATCH, CHANNELS, HEIGHT, WIDTH],
        "output_shape": [BATCH, CHANNELS, OUTPUT_HEIGHT, OUTPUT_WIDTH],
        "tile_sizes": [
            [variant.tile_size, variant.tile_size]
            for variant in variants
            if variant.tile_size is not None
        ],
        "numerical_equivalence": {
            "compiled_variants": "bitwise",
            "torch_reference": {"rtol": 1e-5, "atol": 1e-5},
        },
        "comparability": {
            "input_payload": "identical fingerprint",
            "source_map_origins": "identical",
            "static_access_provenance": "identical",
            "model_buffer_placement": "identical per cache profile",
            "cache_configuration": "identical per cache profile",
            "hierarchy_semantics": "identical exclusive/write-back/write-allocate",
        },
        "stepper_demo": stepper_demo,
        "variants": {},
    }
    for variant in variants:
        name = variant.name
        summary["variants"][name] = {
            "tile_size": variant.tile_size,
            "trace": f"{name}/trace.json",
            "trace_metrics": trace_metrics(traces[name]),
            "demo_cache": simulation_metrics(simulations["demo"][name]),
            "reference_hierarchy": simulation_metrics(
                simulations["reference"][name]
            ),
        }
    baseline = summary["variants"]["baseline"]
    comparisons = {}
    for variant in variants:
        if variant.name == "baseline":
            continue
        candidate = summary["variants"][variant.name]
        demo_baseline_misses = baseline["demo_cache"]["levels"][0]["misses"]
        demo_candidate_misses = candidate["demo_cache"]["levels"][0]["misses"]
        reference_baseline_misses = baseline["reference_hierarchy"]["levels"][0][
            "misses"
        ]
        reference_candidate_misses = candidate["reference_hierarchy"]["levels"][
            0
        ]["misses"]
        comparisons[variant.name] = {
            "event_delta": (
                candidate["trace_metrics"]["events"]
                - baseline["trace_metrics"]["events"]
            ),
            "requested_byte_delta": (
                candidate["trace_metrics"]["requested_bytes"]
                - baseline["trace_metrics"]["requested_bytes"]
            ),
            "demo_l1_miss_delta": demo_candidate_misses - demo_baseline_misses,
            "demo_l1_miss_reduction_percent": (
                100.0
                * (demo_baseline_misses - demo_candidate_misses)
                / demo_baseline_misses
            ),
            "reference_l1_miss_delta": (
                reference_candidate_misses - reference_baseline_misses
            ),
            "reference_l1_miss_reduction_percent": (
                100.0
                * (reference_baseline_misses - reference_candidate_misses)
                / reference_baseline_misses
            ),
            "reference_l2_hit_delta": (
                candidate["reference_hierarchy"]["levels"][1]["hits"]
                - baseline["reference_hierarchy"]["levels"][1]["hits"]
            ),
            "reference_memory_read_delta": (
                candidate["reference_hierarchy"]["memory_reads"]
                - baseline["reference_hierarchy"]["memory_reads"]
            ),
        }
    summary["comparisons"] = comparisons
    tiled_names = [variant.name for variant in variants if variant.tile_size is not None]
    demo_misses = {
        name: summary["variants"][name]["demo_cache"]["levels"][0]["misses"]
        for name in tiled_names
    }
    reference_misses = {
        name: summary["variants"][name]["reference_hierarchy"]["levels"][0][
            "misses"
        ]
        for name in tiled_names
    }
    summary["best_variants"] = {
        "demo_l1_misses": [
            name for name, misses in demo_misses.items()
            if misses == min(demo_misses.values())
        ],
        "reference_l1_misses": [
            name for name, misses in reference_misses.items()
            if misses == min(reference_misses.values())
        ],
    }
    summary_path = args.output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    ui_entries = []
    trace_fields = (
        "schema",
        "variant",
        "total_events",
        "recorded_events",
        "truncated",
        "valid",
        "complete",
    )
    for variant in variants:
        ui_entries.append(
            {
                "path": f"{variant.name}/trace.json",
                "data": {
                    key: traces[variant.name][key]
                    for key in trace_fields
                    if key in traces[variant.name]
                },
            }
        )
        for profile in CACHE_PROFILES:
            ui_entries.append(
                {
                    "path": (
                        f"simulation/{profile}--{variant.name}/statistics.json"
                    ),
                    "data": simulations[profile][variant.name],
                }
            )
    (args.output_dir / "ui-results.json").write_text(
        json.dumps({"entries": ui_entries}) + "\n", encoding="utf-8"
    )

    lines = [
        "# Depthwise temporal blocking results",
        "",
        "Обе реализации вычисляют `DWConv3x3(ReLU(DWConv3x3(input)))`, "
        "совпали bitwise между собой и прошли tolerance-проверку "
        "относительно Torch.",
        "",
        "| Variant | Events | Requested bytes | 16 KiB L1 misses | "
        "32 KiB L1 misses | L2 hits | Memory reads |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for variant in variants:
        item = summary["variants"][variant.name]
        trace = item["trace_metrics"]
        lines.append(
            f"| `{variant.name}` | {trace['events']} | "
            f"{trace['requested_bytes']} | "
            f"{item['demo_cache']['levels'][0]['misses']} | "
            f"{item['reference_hierarchy']['levels'][0]['misses']} | "
            f"{item['reference_hierarchy']['levels'][1]['hits']} | "
            f"{item['reference_hierarchy']['memory_reads']} |"
        )
    lines.extend(["", "## Дельты относительно baseline", ""])
    for variant in variants:
        if variant.name == "baseline":
            continue
        comparison = comparisons[variant.name]
        lines.extend(
            [
                f"### `{variant.name}`",
                "",
                f"- dynamic events: {comparison['event_delta']:+d};",
                f"- requested bytes: {comparison['requested_byte_delta']:+d};",
                f"- 16 KiB L1 misses: {comparison['demo_l1_miss_delta']:+d} "
                f"({comparison['demo_l1_miss_reduction_percent']:.1f}% fewer);",
                f"- 32 KiB L1 misses: {comparison['reference_l1_miss_delta']:+d} "
                f"({comparison['reference_l1_miss_reduction_percent']:.1f}% fewer);",
                f"- reference L2 hits: {comparison['reference_l2_hit_delta']:+d};",
                "- reference hierarchy memory reads: "
                f"{comparison['reference_memory_read_delta']:+d}.",
                "",
            ]
        )
    lines.extend(
        [
            "Temporal blocking выполняет producer и consumer для одного spatial "
            "tile подряд, поэтому промежуточные значения переиспользуются в L1. "
            "Цена — повторное вычисление halo соседних tiles, что увеличивает "
            "число обращений. Sweep показывает этот компромисс для нескольких "
            "размеров tile при неизменных inputs и cache profiles.",
            "",
            "Лучшие варианты по 16 KiB L1 misses: "
            + ", ".join(
                f"`{name}`"
                for name in summary["best_variants"]["demo_l1_misses"]
            )
            + ".",
            "Лучшие варианты по 32 KiB L1 misses: "
            + ", ".join(
                f"`{name}`"
                for name in summary["best_variants"]["reference_l1_misses"]
            )
            + ".",
        ]
    )
    if report is not None:
        lines.extend(["", "Standalone UI: [report.html](report.html)."])
    if stepper_demo is not None:
        lines.extend(
            [
                "Interactive preview: [stepper-demo](stepper-demo/README.md).",
                "Полный сценарий показа: [DEMO.md](DEMO.md).",
            ]
        )
    (args.output_dir / "RESULTS.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary["comparisons"], indent=2))
    print(f"Summary: {summary_path}")
    print(f"Results: {args.output_dir / 'RESULTS.md'}")


def run_experiment(
    args: argparse.Namespace, tile_sizes: tuple[int, ...]
) -> None:
    publish_progress(
        args,
        "preparation",
        "Preparing deterministic inputs and compiler schedules",
    )
    variants = prepare_variants(args.output_dir, tile_sizes)

    inputs, expected = verify_references(tile_sizes)
    write_inputs(args.output_dir, inputs, tile_sizes)
    publish_progress(args, "export", "Exporting the Torch graph to MLIR")
    payload = export_torch_graph(args.output_dir, inputs)
    publish_progress(args, "toolchain", "Checking the CacheScope toolchain")
    plugin, runtime, runner = configure_and_build(args)
    write_experiment_manifest(
        args.output_dir, variants, args, payload, plugin, runtime, runner
    )

    traces: dict[str, dict] = {}
    compiled_outputs: dict[str, torch.Tensor] = {}
    for index, variant in enumerate(variants, 1):
        publish_progress(
            args,
            "compilation",
            f"Compiling and tracing {variant.name}",
            current=index,
            total=len(variants),
            variant=variant.name,
        )
        artifacts = compile_variant(args, variant, payload, plugin, runtime)
        actual, trace = execute_variant(variant, artifacts, inputs)
        torch.testing.assert_close(actual, expected, rtol=1e-5, atol=1e-5)
        compiled_outputs[variant.name] = actual
        traces[variant.name] = trace
        print(
            f"{variant.name}: Torch reference OK, "
            f"events={trace['total_events']}"
        )
    publish_progress(
        args,
        "validation",
        "Validating numerical equivalence and cross-variant provenance",
    )
    for variant in variants:
        torch.testing.assert_close(
            compiled_outputs[variant.name],
            compiled_outputs["baseline"],
            rtol=0.0,
            atol=0.0,
        )
    print("all compiled variants match baseline bitwise")
    assert_comparable_provenance(traces)
    print("cross-variant origins and static access provenance match")

    simulations: dict[str, dict[str, dict]] = {
        profile: {} for profile in CACHE_PROFILES
    }
    simulation_total = len(CACHE_PROFILES) * len(variants)
    simulation_index = 0
    for profile, cache in CACHE_PROFILES.items():
        for variant in variants:
            simulation_index += 1
            publish_progress(
                args,
                "simulation",
                f"Simulating {variant.name} on the {profile} cache profile",
                current=simulation_index,
                total=simulation_total,
                variant=variant.name,
            )
            trace_path = args.output_dir / variant.name / "trace.json"
            simulations[profile][variant.name] = replay(
                runner,
                trace_path,
                args.output_dir
                / "simulation"
                / f"{profile}--{variant.name}",
                cache,
                keep_events=args.keep_events,
            )
    assert_comparable_simulations(simulations)
    print("cache configurations and model buffer placements match")

    publish_progress(args, "stepper", "Building the compact Stepper artifact")
    stepper_demo = build_stepper_bundle(args, variants, traces)
    publish_progress(args, "report", "Building the standalone comparison report")
    report = None if args.skip_ui else build_report_bundle(args, variants)
    publish_progress(args, "results", "Publishing the experiment summary")
    write_demo_guide(args, stepper_demo, report)
    publish_results(args, variants, traces, simulations, report, stepper_demo)


def main() -> int:
    args = parse_args()
    args.llvm_root = args.llvm_root.resolve()
    args.build_dir = args.build_dir.resolve()
    if args.progress_file is not None:
        args.progress_file = args.progress_file.resolve()
    destination = args.output_dir.resolve()
    tile_sizes = parse_tile_sizes(args.tile_sizes)
    if args.stepper_events < 0:
        raise RuntimeError("stepper event count must be non-negative")

    destination.parent.mkdir(parents=True, exist_ok=True)
    validate_output_destination(destination)
    staging = Path(
        tempfile.mkdtemp(
            prefix=f".{destination.name}.staging-",
            dir=destination.parent,
        )
    )
    args.output_dir = staging
    try:
        run_experiment(args, tile_sizes)
        publish_progress(args, "publishing", "Publishing the completed bundle")
        publish_output(staging, destination)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    print(f"Published experiment: {destination}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"depthwise experiment failed: {error}") from error
