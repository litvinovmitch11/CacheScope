#!/usr/bin/env python3
"""Build three compiler variants and produce validated CacheScope traces."""

from __future__ import annotations

import argparse
import itertools
import json
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from validate_trace import validate_trace  # noqa: E402


@dataclass(frozen=True)
class Variant:
    name: str
    transform: Path | None


VARIANTS = (
    Variant("baseline", None),
    Variant("tile4", ROOT / "examples/transforms/matmul-tile4.transform.mlir"),
    Variant(
        "tile4-ikj",
        ROOT / "examples/transforms/matmul-tile4-ikj.transform.mlir",
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compile, instrument, execute, and validate three MLIR variants."
    )
    parser.add_argument(
        "--llvm-build-dir",
        type=Path,
        default=Path(
            os.environ.get("LLVM_BUILD_DIR", ROOT / "llvm-project" / "build")
        ),
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path(os.environ.get("BUILD_DIR", ROOT / "build")),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(
            os.environ.get("CACHESCOPE_OUTPUT_DIR", ROOT / "output")
        )
        / "trace-demo",
    )
    parser.add_argument(
        "--plugin",
        type=Path,
        help="Use an existing CacheScope plugin instead of configuring the project.",
    )
    parser.add_argument(
        "--runtime",
        type=Path,
        help="Use an existing trace runtime together with --plugin.",
    )
    return parser.parse_args()


def run(
    command: list[object],
    *,
    env: dict[str, str] | None = None,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    rendered = [str(argument) for argument in command]
    print("+", shlex.join(rendered), flush=True)
    return subprocess.run(
        rendered,
        cwd=ROOT,
        env=env,
        check=True,
        text=True,
        capture_output=capture,
    )


def require_file(path: Path, description: str) -> Path:
    if not path.is_file():
        raise SystemExit(f"Missing {description}: {path}")
    return path


def find_library(directory: Path, stem: str) -> Path:
    candidates = (
        directory / f"{stem}.so",
        directory / f"{stem}.dylib",
        directory / f"{stem}.dll",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit(f"Missing built library: {candidates[0]}")


def resolve_artifacts(args: argparse.Namespace) -> tuple[Path, Path]:
    if (args.plugin is None) != (args.runtime is None):
        raise SystemExit("--plugin and --runtime must be provided together")
    if args.plugin is not None:
        return (
            require_file(args.plugin.resolve(), "pass plugin"),
            require_file(args.runtime.resolve(), "trace runtime"),
        )

    run(
        [
            "cmake",
            "-S",
            ROOT,
            "-B",
            args.build_dir,
            "-G",
            "Ninja",
            f"-DMLIR_DIR={args.llvm_build_dir / 'lib/cmake/mlir'}",
            "-DCMAKE_BUILD_TYPE=Release",
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
        ]
    )
    plugin = find_library(args.build_dir / "lib", "CacheScopePassPlugin")
    runtime = find_library(
        args.build_dir / "compiler/runtime", "libcachescope_trace_runtime"
    )
    return plugin, runtime


def tool(tool_dir: Path, name: str) -> Path:
    return require_file(tool_dir / name, name)


def compile_variant(
    variant: Variant,
    *,
    payload: Path,
    output_root: Path,
    tool_dir: Path,
    plugin: Path,
    runtime: Path,
) -> dict:
    mlir_opt = tool(tool_dir, "mlir-opt")
    transform_opt = tool(tool_dir, "mlir-transform-opt")
    mlir_runner = tool(tool_dir, "mlir-runner")

    output = output_root / variant.name
    output.mkdir(parents=True, exist_ok=True)
    transformed = output / "01-transformed.mlir"
    loops = output / "02-loops.mlir"
    instrumented = output / "03-instrumented.mlir"
    llvm_mlir = output / "04-llvm.mlir"
    trace_path = output / "trace.json"

    if variant.transform is None:
        run([mlir_opt, payload, "-o", transformed])
    else:
        transformed_text = run(
            [transform_opt, payload, f"--transform={variant.transform}"],
            capture=True,
        ).stdout
        transformed.write_text(transformed_text, encoding="utf-8")

    run(
        [
            mlir_opt,
            transformed,
            "--convert-linalg-to-loops",
            "--canonicalize",
            "--cse",
            "-o",
            loops,
        ]
    )
    run(
        [
            mlir_opt,
            loops,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module(cachescope-instrument-memory{target-func=matmul_kernel},canonicalize,cse)",
            "--mlir-print-debuginfo",
            "-o",
            instrumented,
        ]
    )
    run(
        [
            mlir_opt,
            instrumented,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module(expand-strided-metadata,lower-affine,convert-scf-to-cf,convert-to-llvm,reconcile-unrealized-casts,cachescope-instrument-llvm-fallback{target-func=matmul_kernel})",
            "-o",
            llvm_mlir,
        ]
    )

    environment = os.environ.copy()
    environment.update(
        {
            "CACHESCOPE_TRACE_FILE": str(trace_path),
            "CACHESCOPE_VARIANT": variant.name,
            "CACHESCOPE_PIPELINE_FINGERPRINT": f"trace-demo-{variant.name}-v1",
            "CACHESCOPE_TRACE_LIMIT": "1000000",
        }
    )
    execution = run(
        [
            mlir_runner,
            llvm_mlir,
            "-e",
            "main",
            "-entry-point-result=i32",
            f"--shared-libs={runtime}",
        ],
        env=environment,
        capture=True,
    )
    output_lines = execution.stdout.strip().splitlines()
    if not output_lines or output_lines[-1] != "16":
        raise RuntimeError(
            f"{variant.name}: expected numerical result 16, got {execution.stdout!r}"
        )

    with trace_path.open(encoding="utf-8") as trace_file:
        trace = json.load(trace_file)
    validate_trace(trace)
    if not trace["valid"] or not trace["complete"]:
        raise RuntimeError(f"{variant.name}: trace is invalid or incomplete")
    fallback_events = sum(
        event["address_kind"] == "process_address" for event in trace["events"]
    )
    if fallback_events:
        raise RuntimeError(f"{variant.name}: unexpected LLVM fallback events")

    print(
        f"{variant.name}: result=16, events={trace['total_events']}, "
        f"accesses={len(trace['accesses'])}, trace=valid"
    )
    return trace


def event_key(event: dict) -> tuple:
    return (
        event["access_id"],
        event["memref_id"],
        event["byte_offset"],
        event["size_bytes"],
        event["kind"],
        event["address_kind"],
    )


def access_key(access: dict) -> tuple:
    return (
        access["access_id"],
        access["memref_id"],
        access["kind"],
        access["address_kind"],
    )


def compare_traces(traces: dict[str, dict]) -> dict[str, dict]:
    comparisons: dict[str, dict] = {}
    for left_name, right_name in itertools.combinations(traces, 2):
        left = traces[left_name]
        right = traces[right_name]
        pair = f"{left_name}_vs_{right_name}"
        if [access_key(access) for access in left["accesses"]] != [
            access_key(access) for access in right["accesses"]
        ]:
            raise RuntimeError(f"{pair}: static access tables differ")
        if left["total_events"] != right["total_events"]:
            raise RuntimeError(f"{pair}: event counts differ")
        first_difference = next(
            (
                sequence
                for sequence, (left_event, right_event) in enumerate(
                    zip(left["events"], right["events"], strict=True)
                )
                if event_key(left_event) != event_key(right_event)
            ),
            None,
        )
        if first_difference is None:
            raise RuntimeError(f"{pair}: dynamic traces are identical")
        comparisons[pair] = {"first_dynamic_difference": first_difference}
    return comparisons


def main() -> int:
    args = parse_args()
    args.llvm_build_dir = args.llvm_build_dir.resolve()
    args.build_dir = args.build_dir.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    plugin, runtime = resolve_artifacts(args)
    tool_dir = args.llvm_build_dir / "bin"
    payload = require_file(
        ROOT / "examples/workloads/matmul.mlir", "canonical MLIR payload"
    )

    traces = {
        variant.name: compile_variant(
            variant,
            payload=payload,
            output_root=args.output_dir,
            tool_dir=tool_dir,
            plugin=plugin,
            runtime=runtime,
        )
        for variant in VARIANTS
    }
    comparisons = compare_traces(traces)
    summary = {
        "schema": "cachescope.demo-summary.v1",
        "payload": str(payload.relative_to(ROOT)),
        "variants": {
            name: {
                "trace": f"{name}/trace.json",
                "total_events": trace["total_events"],
                "accesses": len(trace["accesses"]),
                "valid": trace["valid"],
                "complete": trace["complete"],
            }
            for name, trace in traces.items()
        },
        "comparisons": comparisons,
    }
    summary_path = args.output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print("\nCanonical CacheScope demo passed")
    for pair, comparison in comparisons.items():
        print(f"{pair}: first difference at event {comparison['first_dynamic_difference']}")
    print(f"Artifacts: {args.output_dir}")
    print(f"Summary:   {summary_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(
            f"Pipeline command failed with exit code {error.returncode}"
        ) from error
