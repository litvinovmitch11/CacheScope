#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import json
import os
import tempfile
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    return parser.parse_args()


def load_runtime(path: Path) -> ctypes.CDLL:
    library = ctypes.CDLL(str(path))
    semantic = getattr(library, "__cachescope_trace_v1")
    semantic.argtypes = [ctypes.c_int64] * 7
    fallback = getattr(library, "__cachescope_trace_fallback_v1")
    fallback.argtypes = [ctypes.c_int64] * 4
    getattr(library, "__cachescope_trace_reset_v1").argtypes = []
    getattr(library, "__cachescope_trace_flush_v1").argtypes = []
    return library


def read_trace(path: Path) -> dict:
    with path.open(encoding="utf-8") as trace_file:
        return json.load(trace_file)


def main() -> None:
    args = parse_args()
    library = load_runtime(args.runtime)
    semantic = getattr(library, "__cachescope_trace_v1")
    fallback = getattr(library, "__cachescope_trace_fallback_v1")
    reset = getattr(library, "__cachescope_trace_reset_v1")
    flush = getattr(library, "__cachescope_trace_flush_v1")

    with tempfile.TemporaryDirectory(prefix="cachescope-runtime-") as directory:
        output = Path(directory) / "limited.json"
        os.environ["CACHESCOPE_TRACE_FILE"] = str(output)
        os.environ["CACHESCOPE_VARIANT"] = 'runtime "test"'
        os.environ["CACHESCOPE_TRACE_LIMIT"] = "3"
        reset()
        semantic(0, 0, 16, 4, 0, 0, 4)
        semantic(1, 2, 32, 4, 1, 0, 8)
        fallback(2**32, 0x1234, 8, 2)
        semantic(3, 2, 64, 4, 1, 1, 0)
        flush()

        trace = read_trace(output)
        assert trace["schema"] == "cachescope.trace.v1"
        assert trace["variant"] == 'runtime "test"'
        assert trace["total_events"] == 4
        assert trace["recorded_events"] == 3
        assert trace["truncated"] is True
        assert trace["events"][0]["address_kind"] == "memref_offset"
        assert trace["events"][2]["address_kind"] == "process_address"
        assert trace["events"][2]["kind"] == "read_write"

        second_output = Path(directory) / "after-reset.json"
        os.environ["CACHESCOPE_TRACE_FILE"] = str(second_output)
        os.environ["CACHESCOPE_TRACE_LIMIT"] = "10"
        reset()
        semantic(7, 1, 0, 4, 0, 0, 0)
        flush()
        second_trace = read_trace(second_output)
        assert second_trace["total_events"] == 1
        assert second_trace["recorded_events"] == 1
        assert second_trace["truncated"] is False

    print("runtime suite OK")


if __name__ == "__main__":
    main()
