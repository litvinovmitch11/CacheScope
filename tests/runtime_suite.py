#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import json
import os
import sys
import tempfile
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--repo", type=Path, required=True)
    return parser.parse_args()


def load_runtime(path: Path) -> ctypes.CDLL:
    library = ctypes.CDLL(str(path))
    semantic = getattr(library, "__cachescope_trace_v1")
    semantic.argtypes = [ctypes.c_int64] * 5
    fallback = getattr(library, "__cachescope_trace_fallback_v1")
    fallback.argtypes = [ctypes.c_int64] * 4
    register = getattr(library, "__cachescope_register_access_v1")
    register.argtypes = [ctypes.c_int64] * 4
    register_source = getattr(library, "__cachescope_register_source_v1")
    register_source.argtypes = [ctypes.c_int64, ctypes.c_char_p]
    register_source_map = getattr(library, "__cachescope_register_source_map_v1")
    register_source_map.argtypes = [ctypes.c_char_p]
    register_stage = getattr(library, "__cachescope_register_stage_v1")
    register_stage.argtypes = [ctypes.c_int64, ctypes.c_char_p, ctypes.c_char_p]
    register_origin = getattr(library, "__cachescope_register_origin_v1")
    register_origin.argtypes = [
        ctypes.c_int64,
        ctypes.c_char_p,
        ctypes.c_int64,
        ctypes.c_int64,
        ctypes.c_char_p,
    ]
    register_access_origin = getattr(
        library, "__cachescope_register_access_origin_v1"
    )
    register_access_origin.argtypes = [ctypes.c_int64, ctypes.c_int64]
    register_stage_location = getattr(
        library, "__cachescope_register_stage_location_v1"
    )
    register_stage_location.argtypes = [
        ctypes.c_int64,
        ctypes.c_char_p,
        ctypes.c_int64,
        ctypes.c_int64,
        ctypes.c_char_p,
    ]
    getattr(library, "__cachescope_trace_reset_v1").argtypes = []
    getattr(library, "__cachescope_trace_flush_v1").argtypes = []
    return library


def read_trace(path: Path) -> dict:
    with path.open(encoding="utf-8") as trace_file:
        return json.load(trace_file)


def main() -> None:
    args = parse_args()
    sys.path.insert(0, str(args.repo / "scripts"))
    from validate_trace import validate_trace

    library = load_runtime(args.runtime)
    semantic = getattr(library, "__cachescope_trace_v1")
    fallback = getattr(library, "__cachescope_trace_fallback_v1")
    register = getattr(library, "__cachescope_register_access_v1")
    register_source = getattr(library, "__cachescope_register_source_v1")
    register_source_map = getattr(library, "__cachescope_register_source_map_v1")
    register_stage = getattr(library, "__cachescope_register_stage_v1")
    register_origin = getattr(library, "__cachescope_register_origin_v1")
    register_access_origin = getattr(
        library, "__cachescope_register_access_origin_v1"
    )
    register_stage_location = getattr(
        library, "__cachescope_register_stage_location_v1"
    )
    reset = getattr(library, "__cachescope_trace_reset_v1")
    flush = getattr(library, "__cachescope_trace_flush_v1")

    with tempfile.TemporaryDirectory(prefix="cachescope-runtime-") as directory:
        output = Path(directory) / "limited.json"
        os.environ["CACHESCOPE_TRACE_FILE"] = str(output)
        os.environ["CACHESCOPE_VARIANT"] = 'runtime "test"\ncontrol\x01'
        os.environ["CACHESCOPE_PIPELINE_FINGERPRINT"] = "runtime-suite-v1"
        os.environ["CACHESCOPE_TRACE_LIMIT"] = "3"
        reset()
        source_map = {
            "schema": "cachescope.source-map.v1",
            "payload_fingerprint": "sha256:" + "a" * 64,
            "stages": [{"stage_id": "input", "path": "input.mlir"}],
            "origins": [
                {
                    "origin_id": 0,
                    "stage_id": "input",
                    "line": 1,
                    "column": 0,
                    "operation": "test.origin",
                }
            ],
        }
        register_source_map(source_map["payload_fingerprint"].encode())
        register_stage(0, b"input", b"input.mlir")
        register_origin(0, b"input", 1, 0, b"test.origin")
        register(0, 0, 0, 0)
        register_source(0, b'loc("runtime.mlir":1:1)')
        register_access_origin(0, 0)
        register_stage_location(0, b"input", 1, 0, b"memref.load")
        register(1, 2, 1, 0)
        register_source(1, b'loc("runtime.mlir":2:1)')
        register_access_origin(1, 0)
        register_stage_location(1, b"input", 1, 0, b"memref.store")
        register(2**32, -1, 2, 1)
        register_source(2**32, b'loc("runtime.mlir":3:1)')
        register(3, 2, 1, 0)
        register_source(3, b'loc("runtime.mlir":4:1)')
        register_access_origin(3, 0)
        register_stage_location(3, b"input", 1, 0, b"memref.store")
        semantic(0, 0, 16, 4, 0)
        semantic(1, 2, 32, 4, 1)
        fallback(2**32, 0x1234, 8, 2)
        semantic(3, 2, 64, 4, 1)
        flush()

        trace = read_trace(output)
        validate_trace(trace)
        assert trace["schema"] == "cachescope.trace.v1"
        assert trace["variant"] == 'runtime "test"\ncontrol\x01'
        assert trace["producer"]["pipeline_fingerprint"] == "runtime-suite-v1"
        assert trace["valid"] is True
        assert trace["complete"] is False
        assert trace["total_events"] == 4
        assert trace["recorded_events"] == 3
        assert trace["truncated"] is True
        assert trace["events"][0]["address_kind"] == "memref_offset"
        assert trace["events"][2]["address_kind"] == "process_address"
        assert trace["events"][2]["kind"] == "read_write"
        assert trace["source_map"] == source_map
        assert trace["accesses"][0]["origin_ids"] == [0]
        assert trace["accesses"][0]["stage_locations"][0]["column"] == 0
        assert "indices" not in trace["events"][0]
        assert len(trace["accesses"]) == 4
        assert trace["accesses"][3]["access_id"] == 2**32
        assert trace["accesses"][3]["source_location"].startswith("loc(")

        second_output = Path(directory) / "after-reset.json"
        os.environ["CACHESCOPE_TRACE_FILE"] = str(second_output)
        os.environ["CACHESCOPE_TRACE_LIMIT"] = "10"
        reset()
        register(7, 1, 0, 0)
        register_source(7, b'loc("runtime.mlir":7:1)')
        semantic(7, 1, 0, 4, 0)
        flush()
        second_trace = read_trace(second_output)
        validate_trace(second_trace)
        assert second_trace["total_events"] == 1
        assert second_trace["recorded_events"] == 1
        assert second_trace["truncated"] is False
        assert second_trace["valid"] is True
        assert second_trace["complete"] is True
        assert "source_map" not in second_trace

        invalid_limit_output = Path(directory) / "invalid-limit.json"
        os.environ["CACHESCOPE_TRACE_FILE"] = str(invalid_limit_output)
        os.environ["CACHESCOPE_TRACE_LIMIT"] = "-1"
        reset()
        register(8, 1, 0, 0)
        semantic(8, 1, 0, 4, 0)
        flush()
        invalid_limit_trace = read_trace(invalid_limit_output)
        validate_trace(invalid_limit_trace)
        assert invalid_limit_trace["valid"] is False
        assert invalid_limit_trace["errors"] == [
            "CACHESCOPE_TRACE_LIMIT is not an unsigned integer"
        ]

        negative_id_output = Path(directory) / "negative-id.json"
        os.environ["CACHESCOPE_TRACE_FILE"] = str(negative_id_output)
        os.environ["CACHESCOPE_TRACE_LIMIT"] = "10"
        reset()
        register(-1, 0, 0, 0)
        semantic(-1, 0, 0, 4, 0)
        flush()
        negative_id_trace = read_trace(negative_id_output)
        validate_trace(negative_id_trace)
        assert negative_id_trace["valid"] is False
        assert negative_id_trace["events"][0]["access_id"] == -1
        assert negative_id_trace["errors"] == [
            "static access_id must be non-negative",
            "dynamic event references an unregistered access_id",
        ]

        invalid_output = Path(directory) / "invalid.json"
        os.environ["CACHESCOPE_TRACE_FILE"] = str(invalid_output)
        reset()
        register(99, 0, 0, 0)
        semantic(99, 0, 0, 0, 9)
        flush()
        invalid_trace = read_trace(invalid_output)
        validate_trace(invalid_trace)
        assert invalid_trace["valid"] is False
        assert invalid_trace["complete"] is False
        assert invalid_trace["errors"] == [
            "event size_bytes must be positive",
            "event access kind is invalid",
            "dynamic event conflicts with its static access description",
        ]

        orphan_mapping_output = Path(directory) / "orphan-mapping.json"
        os.environ["CACHESCOPE_TRACE_FILE"] = str(orphan_mapping_output)
        reset()
        register(100, 0, 0, 0)
        register_access_origin(100, 0)
        register_stage_location(100, b"input", 1, 0, b"memref.load")
        semantic(100, 0, 0, 4, 0)
        flush()
        orphan_mapping_trace = read_trace(orphan_mapping_output)
        validate_trace(orphan_mapping_trace)
        assert orphan_mapping_trace["valid"] is False
        assert "source_map" not in orphan_mapping_trace
        assert "origin_ids" not in orphan_mapping_trace["accesses"][0]
        assert orphan_mapping_trace["errors"] == [
            "access mapping was registered without a source map"
        ]

        incomplete_mapping_output = Path(directory) / "incomplete-mapping.json"
        os.environ["CACHESCOPE_TRACE_FILE"] = str(incomplete_mapping_output)
        reset()
        register_source_map(source_map["payload_fingerprint"].encode())
        register_stage(0, b"input", b"input.mlir")
        register_origin(0, b"input", 1, 0, b"test.origin")
        register(101, 0, 0, 0)
        semantic(101, 0, 0, 4, 0)
        flush()
        incomplete_mapping_trace = read_trace(incomplete_mapping_output)
        validate_trace(incomplete_mapping_trace)
        assert incomplete_mapping_trace["valid"] is False
        assert "source_map" not in incomplete_mapping_trace
        assert incomplete_mapping_trace["errors"] == [
            "semantic access is missing source mapping metadata"
        ]

        invalid_source_map_output = Path(directory) / "invalid-source-map.json"
        os.environ["CACHESCOPE_TRACE_FILE"] = str(invalid_source_map_output)
        reset()
        register_source_map(b"{")
        register(102, -1, 0, 1)
        fallback(102, 0x1000, 4, 0)
        flush()
        invalid_source_map_trace = read_trace(invalid_source_map_output)
        validate_trace(invalid_source_map_trace)
        assert invalid_source_map_trace["valid"] is False
        assert "source_map" not in invalid_source_map_trace
        assert invalid_source_map_trace["errors"] == [
            "source map payload fingerprint is invalid"
        ]

    print("runtime suite OK")


if __name__ == "__main__":
    main()
