#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from common import run


EXPECTED_STAGES = [
    "input",
    "after-transform",
    "after-linalg-to-loops",
    "after-instrumentation",
    "after-llvm-lowering",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--mlir-bin", type=Path, required=True)
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    return parser.parse_args()


def assert_location(bundle: Path, stages: dict[str, str], location: dict) -> None:
    lines = (bundle / stages[location["stage_id"]]).read_text(
        encoding="utf-8"
    ).splitlines()
    line = lines[location["line"] - 1]
    assert location["column"] < len(line)
    assert location["operation"] in line[location["column"] :]


def assert_source_maps(output: Path) -> None:
    fingerprints: set[str] = set()
    for variant in ("baseline", "tile4", "tile4-ikj"):
        bundle = output / variant
        trace = json.loads((bundle / "trace.json").read_text(encoding="utf-8"))
        source_map = trace["source_map"]
        fingerprints.add(source_map["payload_fingerprint"])
        assert [stage["stage_id"] for stage in source_map["stages"]] == (
            EXPECTED_STAGES
        )
        stages = {
            stage["stage_id"]: stage["path"] for stage in source_map["stages"]
        }
        assert source_map["origins"] == [
            {
                "origin_id": 0,
                "stage_id": "input",
                "line": 3,
                "column": 4,
                "operation": "linalg.matmul",
            }
        ]
        for access in trace["accesses"]:
            assert access["origin_ids"] == [0]
            assert [
                location["stage_id"] for location in access["stage_locations"]
            ] == EXPECTED_STAGES
            for location in access["stage_locations"]:
                assert_location(bundle, stages, location)
    assert len(fingerprints) == 1


def main() -> None:
    args = parse_args()
    with tempfile.TemporaryDirectory(prefix="cachescope-e2e-") as directory:
        output = Path(directory)
        result = run(
            [
                sys.executable,
                args.repo / "examples/trace_demo.py",
                "--llvm-build-dir",
                args.mlir_bin.parent,
                "--output-dir",
                output,
                "--plugin",
                args.plugin,
                "--runtime",
                args.runtime,
            ],
            cwd=args.repo,
        )
        summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
        assert set(summary["variants"]) == {"baseline", "tile4", "tile4-ikj"}
        assert len(summary["comparisons"]) == 3
        assert all(
            variant["valid"] and variant["complete"]
            for variant in summary["variants"].values()
        )
        assert_source_maps(output)

    final_line = result.stdout.strip().splitlines()[-1]
    print(f"end-to-end suite OK; {final_line}")


if __name__ == "__main__":
    main()
