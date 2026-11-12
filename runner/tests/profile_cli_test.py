#!/usr/bin/env python3
"""Exercise the profile through the public CLI and compare explicit geometry."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

runner, trace = sys.argv[1:]
with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)

    def replay(name, *args, succeeds=True):
        output = root / name
        result = subprocess.run(
            [runner, "--trace", trace, "--output-dir", str(output), *args],
            text=True, capture_output=True,
        )
        assert (result.returncode == 0) == succeeds, result.stderr
        if not succeeds:
            assert not output.exists(), "invalid profile must fail before creating output"
            return result
        return [json.loads((output / filename).read_text()) for filename in
                ("statistics.json", "events.json")]

    stats, events = replay("profile", "--cache-profile", "intel-486dx-geometry")
    manual_stats, manual_events = replay(
        "manual", "--size-bytes", "8192", "--line-size-bytes", "16",
        "--associativity", "4", "--replacement-policy", "lru",
    )
    profile = stats.pop("profile")
    assert profile == events.pop("profile")
    assert profile["id"] == "intel-486dx-geometry" and profile["approximate"] is True
    assert profile["source"].startswith("https://") and len(profile["limitations"]) == 3
    assert stats == manual_stats and events == manual_events
    assert stats["cache_config"]["size_bytes"] == 8192
    assert stats["cache_config"]["line_size_bytes"] == 16
    assert stats["cache_config"]["associativity"] == 4
    amd_stats, amd_events = replay("amd", "--cache-profile", "amd-a8-3850-lru")
    explicit_stats, explicit_events = replay(
        "amd-manual", "--cache-level", "L1:65536:64:2:lru",
        "--cache-level", "L2:1048576:64:16:lru",
    )
    amd_profile = amd_stats.pop("profile")
    assert amd_profile == amd_events.pop("profile")
    assert amd_profile["id"] == "amd-a8-3850-lru" and amd_profile["approximate"] is True
    assert "L2 replacement" in amd_profile["limitations"][0]
    assert amd_stats == explicit_stats and amd_events == explicit_events
    assert [level["cache_config"]["size_bytes"] for level in amd_stats["levels"]] == [65536, 1048576]
    assert [level["cache_config"]["associativity"] for level in amd_stats["levels"]] == [2, 16]
    assert amd_stats["hierarchy"]["inclusion_policy"] == "exclusive"
    # Three lines map to one 2-way L1 set, forcing a later L2 hit.
    fixture = json.loads(Path(trace).read_text())
    fixture["accesses"] = [fixture["accesses"][1]]
    fixture["events"] = [
        {**fixture["events"][0], "sequence": i, "byte_offset": offset, "size_bytes": 4}
        for i, offset in enumerate((0, 32768, 65536, 0))
    ]
    fixture["total_events"] = fixture["recorded_events"] = len(fixture["events"])
    trace = str(root / "conflicts.trace.json")
    Path(trace).write_text(json.dumps(fixture))
    conflict_stats, _ = replay("amd-l2-hit", "--cache-profile", "amd-a8-3850-lru")
    assert conflict_stats["levels"][1]["totals"]["hits"] == 1
    assert conflict_stats["levels"][1]["totals"]["misses"] == 3

    for i, option in enumerate(("--size-bytes", "--cache-level", "--random-seed")):
        value = {"--size-bytes": "65536", "--cache-level": "L1:65536:64:2:lru", "--random-seed": "42"}[option]
        replay(f"amd-conflict-{i}", option, value, "--cache-profile", "amd-a8-3850-lru", succeeds=False)
        replay(f"amd-conflict-reversed-{i}", "--cache-profile", "amd-a8-3850-lru", option, value, succeeds=False)
    replay("unknown", "--cache-profile", "intel-486", succeeds=False)
    for i, option in enumerate(("--size-bytes", "--cache-level", "--random-seed")):
        value = {"--size-bytes": "8192", "--cache-level": "L1:8192:16:4:lru", "--random-seed": "42"}[option]
        replay(f"conflict-{i}", option, value, "--cache-profile", "intel-486dx-geometry", succeeds=False)
        replay(f"conflict-reversed-{i}", "--cache-profile", "intel-486dx-geometry", option, value, succeeds=False)
