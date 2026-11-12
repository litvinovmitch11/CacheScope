#!/usr/bin/env python3
"""Build three compiler variants and produce validated CacheScope traces."""

from __future__ import annotations

import argparse
import hashlib
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


def pinned_llvm_root() -> Path:
    version_file = ROOT / "cmake/LLVMVersion.cmake"
    prefix = 'set(CACHESCOPE_LLVM_MAJOR_VERSION "'
    for line in version_file.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix) and line.endswith('")'):
            return Path(f"/usr/lib/llvm-{line[len(prefix) : -2]}")
    raise RuntimeError(f"Cannot read LLVM major version from {version_file}")


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
    cwd: Path = ROOT,
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
            f"-DMLIR_DIR={args.llvm_root / 'lib/cmake/mlir'}",
            f"-DCACHESCOPE_CLANG_TOOLS_DIR={args.llvm_root / 'bin'}",
            f"-DCMAKE_C_COMPILER={args.llvm_root / 'bin/clang'}",
            f"-DCMAKE_CXX_COMPILER={args.llvm_root / 'bin/clang++'}",
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


def compile_stages(
    variant: Variant,
    *,
    payload: Path,
    output_root: Path,
    tool_dir: Path,
    plugin: Path,
    target_function: str = "matmul_kernel",
    lower_affine: bool = False,
) -> tuple[Path, Path]:
    mlir_opt = tool(tool_dir, "mlir-opt")
    transform_opt = tool(tool_dir, "mlir-transform-opt")

    output = output_root / variant.name
    output.mkdir(parents=True, exist_ok=True)
    input_dump = output / "input.mlir"
    input_tagged = output / ".00-input-tagged.mlir"
    transformed_raw = output / ".01-transformed-raw.mlir"
    transformed_tagged = output / ".01-transformed-tagged.mlir"
    loops_tagged = output / ".02-loops-tagged.mlir"
    instrumented_tagged = output / ".03-instrumented-tagged.mlir"
    llvm_tagged = output / ".04-llvm-tagged.mlir"
    executable = output / "05-executable.mlir"
    trace_path = output / "trace.json"

    run(
        [
            mlir_opt,
            payload,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "snapshot-op-locations{filename=input.mlir tag=cachescope.stage.input},"
            f"cachescope-assign-origins{{target-func={target_function}}},"
            f"cachescope-tag-stage{{target-func={target_function} stage-id=input}})",
            "--mlir-print-debuginfo",
            "-o",
            input_tagged.name,
        ],
        cwd=output,
    )

    if variant.transform is None:
        run(
            [
                mlir_opt,
                input_tagged.name,
                "--mlir-print-debuginfo",
                "-o",
                transformed_raw.name,
            ],
            cwd=output,
        )
    else:
        transformed_text = run(
            [
                transform_opt,
                input_tagged.name,
                f"--transform={variant.transform}",
                "--mlir-print-debuginfo",
            ],
            capture=True,
            cwd=output,
        ).stdout
        transformed_raw.write_text(transformed_text, encoding="utf-8")

    run(
        [
            mlir_opt,
            transformed_raw.name,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "snapshot-op-locations{filename=01-transformed.mlir tag=cachescope.stage.after-transform},"
            f"cachescope-tag-stage{{target-func={target_function} stage-id=after-transform}})",
            "--mlir-print-debuginfo",
            "-o",
            transformed_tagged.name,
        ],
        cwd=output,
    )

    run(
        [
            mlir_opt,
            transformed_tagged.name,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            f"convert-linalg-to-loops,{'lower-affine,' if lower_affine else ''}canonicalize,cse,"
            "snapshot-op-locations{filename=02-loops.mlir tag=cachescope.stage.after-linalg-to-loops},"
            f"cachescope-tag-stage{{target-func={target_function} stage-id=after-linalg-to-loops}})",
            "--mlir-print-debuginfo",
            "-o",
            loops_tagged.name,
        ],
        cwd=output,
    )
    run(
        [
            mlir_opt,
            loops_tagged.name,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            f"cachescope-instrument-memory{{target-func={target_function}}},canonicalize,cse,"
            "snapshot-op-locations{filename=03-instrumented.mlir tag=cachescope.stage.after-instrumentation},"
            f"cachescope-tag-stage{{target-func={target_function} stage-id=after-instrumentation}})",
            "--mlir-print-debuginfo",
            "-o",
            instrumented_tagged.name,
        ],
        cwd=output,
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
            f"cachescope-tag-stage{{target-func={target_function} stage-id=after-llvm-lowering}})",
            "--mlir-print-debuginfo",
            "-o",
            llvm_tagged.name,
        ],
        cwd=output,
    )

    payload_fingerprint = "sha256:" + hashlib.sha256(
        input_dump.read_bytes()
    ).hexdigest()
    run(
        [
            mlir_opt,
            llvm_tagged.name,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "cachescope-instrument-llvm-fallback{"
            f"target-func={target_function} payload-fingerprint={payload_fingerprint}"
            "})",
            "-o",
            executable.name,
        ],
        cwd=output,
    )

    return executable, trace_path


def compile_variant(
    variant: Variant,
    *,
    payload: Path,
    output_root: Path,
    tool_dir: Path,
    plugin: Path,
    runtime: Path,
    expected_result: int = 16,
    target_func: str = "matmul_kernel",
) -> dict:
    executable, trace_path = compile_stages(
        variant, payload=payload, output_root=output_root,
        tool_dir=tool_dir, plugin=plugin, target_function=target_func,
    )
    output = output_root / variant.name
    mlir_runner = tool(tool_dir, "mlir-runner")
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
            executable,
            "-e",
            "main",
            "-entry-point-result=i32",
            f"--shared-libs={runtime}",
        ],
        env=environment,
        capture=True,
    )
    output_lines = execution.stdout.strip().splitlines()
    if not output_lines or output_lines[-1] != str(expected_result):
        raise RuntimeError(
            f"{variant.name}: expected numerical result {expected_result}, got {execution.stdout!r}"
        )

    with trace_path.open(encoding="utf-8") as trace_file:
        trace = json.load(trace_file)
    validate_trace(trace, artifact_root=output)
    if not trace["valid"] or not trace["complete"]:
        raise RuntimeError(f"{variant.name}: trace is invalid or incomplete")
    fallback_events = sum(
        event["address_kind"] == "process_address" for event in trace["events"]
    )
    if fallback_events:
        raise RuntimeError(f"{variant.name}: unexpected LLVM fallback events")

    print(
        f"{variant.name}: result={expected_result}, events={trace['total_events']}, "
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
    args.llvm_root = args.llvm_root.resolve()
    args.build_dir = args.build_dir.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    plugin, runtime = resolve_artifacts(args)
    tool_dir = args.llvm_root / "bin"
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
