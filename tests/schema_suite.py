#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from copy import deepcopy
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    return parser.parse_args()


def expect_invalid(validate_trace, error_type, trace: dict, message: str) -> None:
    try:
        validate_trace(trace)
    except error_type:
        return
    raise AssertionError(message)


def main() -> None:
    args = parse_args()
    sys.path.insert(0, str(args.repo / "scripts"))
    from validate_trace import TraceValidationError, validate_trace

    schema_path = args.repo / "schemas/cachescope.trace.v1.schema.json"
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    assert "$id" not in schema
    assert schema["properties"]["schema"]["const"] == "cachescope.trace.v1"
    assert schema["additionalProperties"] is True
    assert (
        schema["$defs"]["source_map"]["properties"]["schema"]["const"]
        == "cachescope.source-map.v1"
    )

    fixtures = args.repo / "tests/fixtures"
    complete = json.loads((fixtures / "trace-v1-complete.json").read_text())
    truncated = json.loads((fixtures / "trace-v1-truncated.json").read_text())
    invalid = json.loads((fixtures / "trace-v1-invalid.json").read_text())
    source_map_bundle = fixtures / "source-map-v1"
    source_mapped = json.loads((source_map_bundle / "trace.json").read_text())
    validate_trace(complete)
    validate_trace(truncated)
    validate_trace(invalid)
    validate_trace(source_mapped)
    def validate_source_map_bundle(trace: dict) -> None:
        validate_trace(trace, artifact_root=source_map_bundle)

    validate_source_map_bundle(source_mapped)

    wrong_schema = deepcopy(complete)
    wrong_schema["schema"] = "cachescope.trace.v2"
    expect_invalid(
        validate_trace,
        TraceValidationError,
        wrong_schema,
        "validator accepted an unknown schema",
    )

    inconsistent = deepcopy(complete)
    inconsistent["complete"] = False
    expect_invalid(
        validate_trace,
        TraceValidationError,
        inconsistent,
        "validator accepted inconsistent completion flags",
    )

    impossible_counts = deepcopy(complete)
    impossible_counts["total_events"] = 0
    expect_invalid(
        validate_trace,
        TraceValidationError,
        impossible_counts,
        "validator accepted total_events smaller than recorded_events",
    )

    wrong_event_type = deepcopy(complete)
    wrong_event_type["events"][0]["access_id"] = []
    expect_invalid(
        validate_trace,
        TraceValidationError,
        wrong_event_type,
        "validator accepted a non-integer event access_id",
    )

    diagnostic = deepcopy(invalid)
    diagnostic["events"][0]["access_id"] = -1
    validate_trace(diagnostic)

    absolute_stage_path = deepcopy(source_mapped)
    absolute_stage_path["source_map"]["stages"][0]["path"] = "/tmp/input.mlir"
    expect_invalid(
        validate_trace,
        TraceValidationError,
        absolute_stage_path,
        "validator accepted an absolute stage path",
    )

    wrong_fingerprint = deepcopy(source_mapped)
    wrong_fingerprint["source_map"]["payload_fingerprint"] = "sha256:" + "0" * 64
    expect_invalid(
        validate_source_map_bundle,
        TraceValidationError,
        wrong_fingerprint,
        "validator accepted a fingerprint that does not match the input dump",
    )

    unknown_origin = deepcopy(source_mapped)
    unknown_origin["accesses"][0]["origin_ids"] = [99]
    expect_invalid(
        validate_trace,
        TraceValidationError,
        unknown_origin,
        "validator accepted an unknown origin_id",
    )

    unknown_stage = deepcopy(source_mapped)
    unknown_stage["accesses"][0]["stage_locations"][0]["stage_id"] = "missing"
    expect_invalid(
        validate_trace,
        TraceValidationError,
        unknown_stage,
        "validator accepted an unknown stage_id",
    )

    missing_semantic_mapping = deepcopy(source_mapped)
    del missing_semantic_mapping["accesses"][0]["stage_locations"]
    expect_invalid(
        validate_trace,
        TraceValidationError,
        missing_semantic_mapping,
        "validator accepted a semantic access without stage locations",
    )

    unscoped_mapping = deepcopy(complete)
    unscoped_mapping["accesses"][0]["origin_ids"] = [0]
    expect_invalid(
        validate_trace,
        TraceValidationError,
        unscoped_mapping,
        "validator accepted mapping fields without source_map",
    )
    print("schema suite OK")


if __name__ == "__main__":
    main()
