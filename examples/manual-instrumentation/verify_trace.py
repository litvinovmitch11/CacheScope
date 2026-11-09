#!/usr/bin/env python3

import json
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"trace verification failed: {message}")


if len(sys.argv) != 2:
    raise SystemExit(f"usage: {Path(sys.argv[0]).name} TRACE.json")

trace_path = Path(sys.argv[1])
with trace_path.open(encoding="utf-8") as trace_file:
    trace = json.load(trace_file)

if trace.get("schema") != "cachescope.trace.v1":
    fail("unexpected schema")
if trace.get("total_events") != 16 * 16 * 16 * 4:
    fail(f"expected 16384 events, got {trace.get('total_events')}")
if trace.get("truncated"):
    fail("trace is unexpectedly truncated")

events = trace.get("events", [])
expected_prefix = [
    (0, 0, 0, "read", [0, 0]),
    (1, 1, 0, "read", [0, 0]),
    (2, 2, 0, "read", [0, 0]),
    (3, 2, 0, "write", [0, 0]),
    (0, 0, 4, "read", [0, 1]),
    (1, 1, 64, "read", [1, 0]),
    (2, 2, 0, "read", [0, 0]),
    (3, 2, 0, "write", [0, 0]),
]

if len(events) < len(expected_prefix):
    fail("trace does not contain the expected prefix")

for sequence, expected in enumerate(expected_prefix):
    event = events[sequence]
    actual = (
        event.get("access_id"),
        event.get("memref_id"),
        event.get("byte_offset"),
        event.get("kind"),
        event.get("indices"),
    )
    if actual != expected:
        fail(f"event {sequence}: expected {expected}, got {actual}")
    if event.get("sequence") != sequence:
        fail(f"event {sequence} has a non-contiguous sequence number")

print(
    f"trace OK: {trace['total_events']} events, "
    f"first {len(expected_prefix)} offsets and indices verified"
)
