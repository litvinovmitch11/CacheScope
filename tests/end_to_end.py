#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import tempfile
from pathlib import Path

from common import require_tool, run


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--mlir-bin", type=Path, required=True)
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    return parser.parse_args()


def event_key(event: dict) -> tuple:
    return (
        event["access_id"],
        event["memref_id"],
        event["byte_offset"],
        event["kind"],
        tuple(event["indices"]),
    )


def build_variant(
    *,
    name: str,
    payload: Path,
    transform: Path | None,
    output: Path,
    mlir_opt: Path,
    transform_opt: Path,
    mlir_runner: Path,
    plugin: Path,
    runtime: Path,
) -> dict:
    transformed = output / f"{name}.transformed.mlir"
    loops = output / f"{name}.loops.mlir"
    marked = output / f"{name}.instrumented.mlir"
    llvm_mlir = output / f"{name}.llvm.mlir"
    trace_path = output / f"{name}.trace.json"

    if transform is None:
        run([mlir_opt, payload, "-o", transformed])
    else:
        transformed_text = run(
            [transform_opt, payload, f"--transform={transform}"]
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
            marked,
        ]
    )
    run(
        [
            mlir_opt,
            marked,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module(expand-strided-metadata,lower-affine,convert-scf-to-cf,convert-to-llvm,reconcile-unrealized-casts,cachescope-instrument-llvm-fallback{target-func=matmul_kernel})",
            "-o",
            llvm_mlir,
        ]
    )

    environment = os.environ.copy()
    environment["CACHESCOPE_TRACE_FILE"] = str(trace_path)
    environment["CACHESCOPE_VARIANT"] = name
    result = run(
        [
            mlir_runner,
            llvm_mlir,
            "-e",
            "main",
            "-entry-point-result=i32",
            f"--shared-libs={runtime}",
        ],
        env=environment,
    )
    if result.stdout.strip().splitlines()[-1] != "16":
        raise AssertionError(f"{name}: unexpected result: {result.stdout!r}")

    with trace_path.open(encoding="utf-8") as trace_file:
        trace = json.load(trace_file)
    if trace["total_events"] != 16384:
        raise AssertionError(f"{name}: unexpected event count")
    if any(
        event.get("address_kind") != "memref_offset"
        for event in trace["events"]
    ):
        raise AssertionError(f"{name}: unexpected LLVM fallback event")
    return trace


def main() -> None:
    args = parse_args()
    mlir_opt = require_tool(args.mlir_bin, "mlir-opt")
    transform_opt = require_tool(args.mlir_bin, "mlir-transform-opt")
    mlir_runner = require_tool(args.mlir_bin, "mlir-runner")
    examples = args.repo / "examples/live-demo"

    with tempfile.TemporaryDirectory(prefix="cachescope-e2e-") as directory:
        output = Path(directory)
        baseline = build_variant(
            name="baseline",
            payload=examples / "matmul.mlir",
            transform=None,
            output=output,
            mlir_opt=mlir_opt,
            transform_opt=transform_opt,
            mlir_runner=mlir_runner,
            plugin=args.plugin,
            runtime=args.runtime,
        )
        tiled = build_variant(
            name="tile4-ikj",
            payload=examples / "matmul.mlir",
            transform=examples / "matmul-tile4-ikj.transform.mlir",
            output=output,
            mlir_opt=mlir_opt,
            transform_opt=transform_opt,
            mlir_runner=mlir_runner,
            plugin=args.plugin,
            runtime=args.runtime,
        )

        assert baseline["accesses"] == tiled["accesses"]
        first_difference = next(
            (
                sequence
                for sequence, (left, right) in enumerate(
                    zip(baseline["events"], tiled["events"], strict=True)
                )
                if event_key(left) != event_key(right)
            ),
            None,
        )
        if first_difference is None:
            raise AssertionError("baseline and tiled traces are identical")

    print(f"end-to-end suite OK; first difference at {first_difference}")


if __name__ == "__main__":
    main()
