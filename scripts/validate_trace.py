#!/usr/bin/env python3
"""Validate CacheScope trace.v1 structure and cross-field invariants."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


KINDS = {"read", "write", "read_write"}
SERIALIZED_KINDS = KINDS | {"invalid"}
ADDRESS_KINDS = {"memref_offset", "process_address"}


class TraceValidationError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TraceValidationError(message)


def require_keys(value: dict[str, Any], keys: tuple[str, ...], context: str) -> None:
    missing = [key for key in keys if key not in value]
    require(not missing, f"{context}: missing fields: {', '.join(missing)}")


def validate_trace(trace: Any) -> None:
    require(isinstance(trace, dict), "trace must be a JSON object")
    required = (
        "schema",
        "producer",
        "variant",
        "valid",
        "complete",
        "total_events",
        "recorded_events",
        "truncated",
        "errors",
        "accesses",
        "events",
    )
    require_keys(trace, required, "trace")
    require(trace["schema"] == "cachescope.trace.v1", "unsupported trace schema")

    producer = trace["producer"]
    require(isinstance(producer, dict), "producer must be an object")
    producer_fields = (
        "name",
        "version",
        "llvm_version",
        "llvm_revision",
        "pipeline_fingerprint",
    )
    require_keys(producer, producer_fields, "producer")
    require(producer["name"] == "CacheScope", "unexpected producer name")
    for field in producer_fields:
        require(isinstance(producer[field], str), f"producer.{field} must be a string")

    for field in ("valid", "complete", "truncated"):
        require(type(trace[field]) is bool, f"{field} must be a boolean")
    for field in ("total_events", "recorded_events"):
        require(
            type(trace[field]) is int and trace[field] >= 0,
            f"{field} must be a non-negative integer",
        )
    require(isinstance(trace["variant"], str), "variant must be a string")
    require(
        isinstance(trace["errors"], list)
        and all(isinstance(error, str) for error in trace["errors"]),
        "errors must be an array of strings",
    )
    require(isinstance(trace["accesses"], list), "accesses must be an array")
    require(isinstance(trace["events"], list), "events must be an array")

    require(trace["valid"] == (not trace["errors"]), "valid/errors invariant failed")
    require(
        trace["truncated"]
        == (trace["total_events"] > trace["recorded_events"]),
        "truncated/event counts invariant failed",
    )
    require(
        trace["complete"] == (trace["valid"] and not trace["truncated"]),
        "complete invariant failed",
    )
    require(
        trace["recorded_events"] == len(trace["events"]),
        "recorded_events does not match events length",
    )
    require(
        trace["total_events"] >= trace["recorded_events"],
        "total_events must not be smaller than recorded_events",
    )

    accesses: dict[int, dict[str, Any]] = {}
    access_fields = (
        "access_id",
        "memref_id",
        "kind",
        "address_kind",
        "source_location",
    )
    for index, access in enumerate(trace["accesses"]):
        context = f"accesses[{index}]"
        require(isinstance(access, dict), f"{context} must be an object")
        require_keys(access, access_fields, context)
        require(
            type(access["access_id"]) is int and access["access_id"] >= 0,
            f"{context}.access_id must be a non-negative integer",
        )
        require(type(access["memref_id"]) is int, f"{context}.memref_id must be an integer")
        require(isinstance(access["kind"], str), f"{context}.kind must be a string")
        require(access["kind"] in KINDS, f"{context}.kind is invalid")
        require(
            isinstance(access["address_kind"], str)
            and access["address_kind"] in ADDRESS_KINDS,
            f"{context}.address_kind is invalid",
        )
        require(
            isinstance(access["source_location"], str),
            f"{context}.source_location must be a string",
        )
        require(access["access_id"] not in accesses, f"duplicate access_id {access['access_id']}")
        if access["address_kind"] == "memref_offset":
            require(access["memref_id"] >= 0, f"{context} has invalid semantic memref_id")
        else:
            require(access["memref_id"] == -1, f"{context} has invalid fallback memref_id")
        accesses[access["access_id"]] = access

    event_fields = (
        "sequence",
        "access_id",
        "memref_id",
        "byte_offset",
        "size_bytes",
        "kind",
        "address_kind",
    )
    for index, event in enumerate(trace["events"]):
        context = f"events[{index}]"
        require(isinstance(event, dict), f"{context} must be an object")
        require_keys(event, event_fields, context)
        require(
            type(event["sequence"]) is int and event["sequence"] >= 0,
            f"{context}.sequence must be a non-negative integer",
        )
        require(event["sequence"] == index, f"{context}.sequence is not contiguous")
        require(
            type(event["access_id"]) is int,
            f"{context}.access_id must be an integer",
        )
        require(
            event["access_id"] >= 0 or not trace["valid"],
            f"{context}.access_id must be non-negative in a valid trace",
        )
        require(
            type(event["memref_id"]) is int,
            f"{context}.memref_id must be an integer",
        )
        require(type(event["byte_offset"]) is int, f"{context}.byte_offset must be an integer")
        require(
            type(event["size_bytes"]) is int,
            f"{context}.size_bytes must be an integer",
        )
        require(
            event["size_bytes"] > 0 or not trace["valid"],
            f"{context}.size_bytes must be positive in a valid trace",
        )
        require(isinstance(event["kind"], str), f"{context}.kind must be a string")
        require(event["kind"] in SERIALIZED_KINDS, f"{context}.kind is invalid")
        require(
            event["kind"] in KINDS or not trace["valid"],
            f"{context}.kind is invalid in a valid trace",
        )
        require(
            isinstance(event["address_kind"], str)
            and event["address_kind"] in ADDRESS_KINDS,
            f"{context}.address_kind is invalid",
        )
        access = accesses.get(event["access_id"])
        if access is None:
            require(
                not trace["valid"],
                f"{context} references an unknown access_id",
            )
            continue
        matches_access = (
            event["memref_id"],
            event["kind"],
            event["address_kind"],
        ) == (access["memref_id"], access["kind"], access["address_kind"])
        require(
            matches_access or not trace["valid"],
            f"{context} conflicts with its static access description",
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    with args.trace.open(encoding="utf-8") as trace_file:
        trace = json.load(trace_file)
    try:
        validate_trace(trace)
    except TraceValidationError as error:
        raise SystemExit(f"{args.trace}: {error}") from error
    print(f"{args.trace}: valid cachescope.trace.v1")


if __name__ == "__main__":
    main()
