#!/usr/bin/env python3
"""Validate CacheScope trace.v1 structure and cross-field invariants."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any


KINDS = {"read", "write", "read_write"}
SERIALIZED_KINDS = KINDS | {"invalid"}
ADDRESS_KINDS = {"memref_offset", "process_address"}
SOURCE_MAP_SCHEMA = "cachescope.source-map.v1"
STAGE_ID_PATTERN = re.compile(r"^[a-z][a-z0-9-]*$")
PAYLOAD_FINGERPRINT_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")


class TraceValidationError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TraceValidationError(message)


def require_keys(value: dict[str, Any], keys: tuple[str, ...], context: str) -> None:
    missing = [key for key in keys if key not in value]
    require(not missing, f"{context}: missing fields: {', '.join(missing)}")


def validate_stage_id(value: Any, context: str) -> str:
    require(
        isinstance(value, str) and STAGE_ID_PATTERN.fullmatch(value) is not None,
        f"{context} must match {STAGE_ID_PATTERN.pattern}",
    )
    return value


def validate_artifact_path(value: Any, context: str) -> str:
    require(isinstance(value, str) and value, f"{context} must be a string")
    require("\\" not in value, f"{context} must use forward slashes")
    require(not value.startswith("/"), f"{context} must be relative")
    require(
        all(part not in {"", ".", ".."} for part in value.split("/")),
        f"{context} must not contain empty, dot, or parent components",
    )
    return value


def validate_source_map(
    source_map: Any, artifact_root: Path | None = None
) -> tuple[set[str], set[int]]:
    require(isinstance(source_map, dict), "source_map must be an object")
    require_keys(
        source_map,
        ("schema", "payload_fingerprint", "stages", "origins"),
        "source_map",
    )
    require(
        source_map["schema"] == SOURCE_MAP_SCHEMA,
        "unsupported source_map schema",
    )
    require(
        isinstance(source_map["payload_fingerprint"], str)
        and PAYLOAD_FINGERPRINT_PATTERN.fullmatch(
            source_map["payload_fingerprint"]
        )
        is not None,
        "source_map.payload_fingerprint must be a lowercase sha256 digest",
    )
    require(
        isinstance(source_map["stages"], list) and source_map["stages"],
        "source_map.stages must be a non-empty array",
    )
    require(
        isinstance(source_map["origins"], list),
        "source_map.origins must be an array",
    )

    stage_ids: set[str] = set()
    stage_paths: set[str] = set()
    stage_paths_by_id: dict[str, str] = {}
    for index, stage in enumerate(source_map["stages"]):
        context = f"source_map.stages[{index}]"
        require(isinstance(stage, dict), f"{context} must be an object")
        require_keys(stage, ("stage_id", "path"), context)
        stage_id = validate_stage_id(stage["stage_id"], f"{context}.stage_id")
        path = validate_artifact_path(stage["path"], f"{context}.path")
        require(stage_id not in stage_ids, f"duplicate stage_id {stage_id}")
        require(path not in stage_paths, f"duplicate stage path {path}")
        stage_ids.add(stage_id)
        stage_paths.add(path)
        stage_paths_by_id[stage_id] = path

    require(
        source_map["stages"][0]["stage_id"] == "input",
        "source_map.stages must begin with the input stage",
    )

    if artifact_root is not None:
        resolved_root = artifact_root.resolve()
        for stage_id, path in stage_paths_by_id.items():
            artifact = (artifact_root / path).resolve()
            require(
                artifact.is_relative_to(resolved_root),
                f"source_map stage {stage_id} escapes the trace directory",
            )
            require(
                artifact.is_file(),
                f"source_map stage {stage_id} does not exist: {path}",
            )
        input_artifact = artifact_root / stage_paths_by_id["input"]
        fingerprint = f"sha256:{hashlib.sha256(input_artifact.read_bytes()).hexdigest()}"
        require(
            source_map["payload_fingerprint"] == fingerprint,
            "source_map.payload_fingerprint does not match the input stage bytes",
        )

    origin_ids: set[int] = set()
    for index, origin in enumerate(source_map["origins"]):
        context = f"source_map.origins[{index}]"
        require(isinstance(origin, dict), f"{context} must be an object")
        require_keys(
            origin,
            ("origin_id", "stage_id", "line", "column", "operation"),
            context,
        )
        require(
            type(origin["origin_id"]) is int and origin["origin_id"] >= 0,
            f"{context}.origin_id must be a non-negative integer",
        )
        require(
            origin["origin_id"] not in origin_ids,
            f"duplicate origin_id {origin['origin_id']}",
        )
        stage_id = validate_stage_id(origin["stage_id"], f"{context}.stage_id")
        require(stage_id in stage_ids, f"{context} references an unknown stage_id")
        require(stage_id == "input", f"{context} must reference the input stage")
        require(
            type(origin["line"]) is int and origin["line"] >= 1,
            f"{context}.line must be a positive integer",
        )
        require(
            type(origin["column"]) is int and origin["column"] >= 0,
            f"{context}.column must be a non-negative integer",
        )
        require(
            isinstance(origin["operation"], str) and origin["operation"],
            f"{context}.operation must be a non-empty string",
        )
        origin_ids.add(origin["origin_id"])

    return stage_ids, origin_ids


def validate_trace(trace: Any, artifact_root: Path | None = None) -> None:
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

    source_map = trace.get("source_map")
    if source_map is None:
        source_map_stage_ids: set[str] = set()
        source_map_origin_ids: set[int] = set()
    else:
        source_map_stage_ids, source_map_origin_ids = validate_source_map(
            source_map, artifact_root
        )

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

        has_origin_ids = "origin_ids" in access
        has_stage_locations = "stage_locations" in access
        if source_map is None:
            require(
                not has_origin_ids and not has_stage_locations,
                f"{context} has source mapping fields without source_map",
            )
        else:
            if access["address_kind"] == "memref_offset":
                require(has_origin_ids, f"{context} is missing origin_ids")
                require(has_stage_locations, f"{context} is missing stage_locations")

            if has_origin_ids:
                origin_ids = access["origin_ids"]
                require(
                    isinstance(origin_ids, list),
                    f"{context}.origin_ids must be an array",
                )
                if access["address_kind"] == "memref_offset":
                    require(origin_ids, f"{context}.origin_ids must not be empty")
                for origin_id in origin_ids:
                    require(
                        type(origin_id) is int and origin_id >= 0,
                        f"{context}.origin_ids must contain non-negative integers",
                    )
                    require(
                        origin_id in source_map_origin_ids,
                        f"{context} references an unknown origin_id",
                    )
                require(
                    len(origin_ids) == len(set(origin_ids)),
                    f"{context}.origin_ids contains duplicates",
                )

            if has_stage_locations:
                stage_locations = access["stage_locations"]
                require(
                    isinstance(stage_locations, list),
                    f"{context}.stage_locations must be an array",
                )
                if access["address_kind"] == "memref_offset":
                    require(
                        stage_locations,
                        f"{context}.stage_locations must not be empty",
                    )
                for location_index, location in enumerate(stage_locations):
                    location_context = (
                        f"{context}.stage_locations[{location_index}]"
                    )
                    require(
                        isinstance(location, dict),
                        f"{location_context} must be an object",
                    )
                    require_keys(
                        location,
                        ("stage_id", "line", "column", "operation"),
                        location_context,
                    )
                    stage_id = validate_stage_id(
                        location["stage_id"], f"{location_context}.stage_id"
                    )
                    require(
                        stage_id in source_map_stage_ids,
                        f"{location_context} references an unknown stage_id",
                    )
                    require(
                        type(location["line"]) is int and location["line"] >= 1,
                        f"{location_context}.line must be a positive integer",
                    )
                    require(
                        type(location["column"]) is int
                        and location["column"] >= 0,
                        f"{location_context}.column must be a non-negative integer",
                    )
                    require(
                        isinstance(location["operation"], str)
                        and location["operation"],
                        f"{location_context}.operation must be a non-empty string",
                    )
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
        validate_trace(trace, artifact_root=args.trace.parent)
    except TraceValidationError as error:
        raise SystemExit(f"{args.trace}: {error}") from error
    print(f"{args.trace}: valid cachescope.trace.v1")


if __name__ == "__main__":
    main()
