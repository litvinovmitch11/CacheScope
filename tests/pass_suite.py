#!/usr/bin/env python3
from __future__ import annotations

import argparse
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


def filecheck(file_check: Path, source: Path, text: str) -> None:
    result = run([file_check, source], input_text=text, check=False)
    if result.returncode != 0:
        raise AssertionError(result.stderr or result.stdout)


def positive_case(
    mlir_opt: Path,
    file_check: Path,
    plugin: Path,
    source: Path,
    pipeline: str,
) -> None:
    result = run(
        [
            mlir_opt,
            source,
            f"--load-pass-plugin={plugin}",
            f"--pass-pipeline={pipeline}",
        ]
    )
    filecheck(file_check, source, result.stdout)


def negative_case(
    mlir_opt: Path,
    file_check: Path,
    plugin: Path,
    source: Path,
    pipeline: str,
) -> None:
    result = run(
        [
            mlir_opt,
            source,
            f"--load-pass-plugin={plugin}",
            f"--pass-pipeline={pipeline}",
        ],
        check=False,
    )
    if result.returncode == 0:
        raise AssertionError(f"expected failure for {source.name}")
    filecheck(file_check, source, result.stderr)


def test_marker_survives_lowering(
    mlir_opt: Path, plugin: Path, source: Path, temporary: Path
) -> None:
    marked = temporary / "marked.mlir"
    converted = temporary / "converted.mlir"
    lowered = temporary / "lowered.mlir"
    run(
        [
            mlir_opt,
            source,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module(cachescope-instrument-memory{target-func=kernel})",
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
            "--pass-pipeline=builtin.module(expand-strided-metadata,lower-affine,convert-scf-to-cf,convert-to-llvm,reconcile-unrealized-casts)",
            "--mlir-print-debuginfo",
            "-o",
            converted,
        ]
    )
    run(
        [
            mlir_opt,
            converted,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})",
            "-o",
            lowered,
        ]
    )
    text = lowered.read_text(encoding="utf-8")
    if "cachescope.llvm_fallback_instrumented" not in text:
        raise AssertionError("LLVM coverage pass did not run")
    if "llvm.load" not in text or "llvm.store" not in text:
        raise AssertionError("expected lowered LLVM memory operations")
    if "__cachescope_trace_fallback_v1" in text:
        raise AssertionError("covered MemRef accesses were instrumented twice")
    if "__cachescope_register_access_v1" not in text:
        raise AssertionError("static access registrations are missing")
    if "__cachescope_register_source_v1" not in text:
        raise AssertionError("source location registrations are missing")
    if "__cachescope_source_0" not in text:
        raise AssertionError("source location table is missing")

    incomplete_mapping = run(
        [
            mlir_opt,
            converted,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            "cachescope-instrument-llvm-fallback{"
            f"target-func=kernel payload-fingerprint=sha256:{'0' * 64}"
            "})",
        ],
        check=False,
    )
    if incomplete_mapping.returncode == 0:
        raise AssertionError("incomplete source mapping was accepted")
    if "cannot build a complete source map" not in incomplete_mapping.stderr:
        raise AssertionError("missing fail-closed source mapping diagnostic")


def test_source_mapping_markers(
    mlir_opt: Path, plugin: Path, source: Path, temporary: Path
) -> None:
    snapshot = temporary / "input.mlir"
    result = run(
        [
            mlir_opt,
            source,
            f"--load-pass-plugin={plugin}",
            "--pass-pipeline=builtin.module("
            f"snapshot-op-locations{{filename={snapshot} tag=cachescope.stage.input}},"
            "cachescope-assign-origins{target-func=kernel},"
            "cachescope-tag-stage{target-func=kernel stage-id=input})",
            "--mlir-print-debuginfo",
        ]
    )
    for marker in (
        "cachescope.origin_id",
        "cachescope.origin_operation",
        "cachescope.stage.input",
        "cachescope.stage_id",
        "cachescope.stage_operation",
    ):
        if marker not in result.stdout:
            raise AssertionError(f"source mapping marker is missing: {marker}")
    if not snapshot.is_file():
        raise AssertionError("source mapping input snapshot is missing")


def main() -> None:
    args = parse_args()
    tests = args.repo / "compiler/cs-trace/test"
    mlir_opt = require_tool(args.mlir_bin, "mlir-opt")
    file_check = require_tool(args.mlir_bin, "FileCheck")

    semantic_pipeline = (
        "builtin.module(cachescope-instrument-memory{target-func=kernel})"
    )
    fallback_pipeline = (
        "builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})"
    )

    positive_case(
        mlir_opt,
        file_check,
        args.plugin,
        tests / "instrument-memory.mlir",
        semantic_pipeline,
    )
    positive_case(
        mlir_opt,
        file_check,
        args.plugin,
        tests / "arbitrary-rank.mlir",
        semantic_pipeline,
    )
    positive_case(
        mlir_opt,
        file_check,
        args.plugin,
        tests / "instrument-llvm-fallback.mlir",
        fallback_pipeline,
    )
    negative_case(
        mlir_opt,
        file_check,
        args.plugin,
        tests / "unsupported-llvm-memory.mlir",
        fallback_pipeline,
    )
    negative_case(
        mlir_opt,
        file_check,
        args.plugin,
        tests / "unknown-llvm-call.mlir",
        fallback_pipeline,
    )

    with tempfile.TemporaryDirectory(prefix="cachescope-pass-") as directory:
        temporary = Path(directory)
        test_source_mapping_markers(
            mlir_opt,
            args.plugin,
            tests / "instrument-memory.mlir",
            temporary,
        )
        test_marker_survives_lowering(
            mlir_opt,
            args.plugin,
            tests / "instrument-memory.mlir",
            temporary,
        )

    print("pass suite OK")


if __name__ == "__main__":
    main()
