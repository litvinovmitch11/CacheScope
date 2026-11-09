#!/usr/bin/env python3

import json
import sys
from pathlib import Path


if len(sys.argv) != 3:
    raise SystemExit(f"usage: {Path(sys.argv[0]).name} BASELINE.json TILED.json")


def load(path: str) -> dict:
    with Path(path).open(encoding="utf-8") as trace_file:
        return json.load(trace_file)


def event_key(event: dict) -> tuple:
    return (
        event["access_id"],
        event["memref_id"],
        event["byte_offset"],
        event["kind"],
        tuple(event["indices"]),
    )


baseline = load(sys.argv[1])
tiled = load(sys.argv[2])

if baseline["accesses"] != tiled["accesses"]:
    raise SystemExit("trace comparison failed: static access tables differ")
if baseline["total_events"] != tiled["total_events"]:
    raise SystemExit("trace comparison failed: event counts differ")

for sequence, (left, right) in enumerate(
    zip(baseline["events"], tiled["events"], strict=True)
):
    if event_key(left) != event_key(right):
        print(
            "trace comparison OK: static access IDs match; "
            f"dynamic order first differs at sequence {sequence}"
        )
        print(f"  baseline: {event_key(left)}")
        print(f"  tiled:    {event_key(right)}")
        break
else:
    raise SystemExit("trace comparison failed: variants have identical event order")
