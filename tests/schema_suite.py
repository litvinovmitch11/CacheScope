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

    fixtures = args.repo / "tests/fixtures"
    complete = json.loads((fixtures / "trace-v1-complete.json").read_text())
    truncated = json.loads((fixtures / "trace-v1-truncated.json").read_text())
    invalid = json.loads((fixtures / "trace-v1-invalid.json").read_text())
    validate_trace(complete)
    validate_trace(truncated)
    validate_trace(invalid)

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
    print("schema suite OK")


if __name__ == "__main__":
    main()
