#!/usr/bin/env python3
"""Run a 128 KiB MLIR workload through the A8-3850 exclusive cache profile."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "examples"))
import trace_demo  # noqa: E402

spec = importlib.util.spec_from_file_location("product_e2e", ROOT / "examples/product-e2e/run.py")
assert spec is not None and spec.loader is not None
product = importlib.util.module_from_spec(spec)
spec.loader.exec_module(product)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-root", type=Path, default=Path(os.environ.get("LLVM_ROOT", trace_demo.pinned_llvm_root())))
    parser.add_argument("--build-dir", type=Path, default=Path(os.environ.get("BUILD_DIR", ROOT / "build")))
    parser.add_argument("--output-dir", type=Path, default=Path(os.environ.get("CACHESCOPE_OUTPUT_DIR", ROOT / "output")) / "a8-l2")
    args = parser.parse_args()
    args.llvm_root = args.llvm_root.resolve()
    args.build_dir = args.build_dir.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    plugin, runtime, runner = product.configure_and_build(args)
    trace = trace_demo.compile_variant(
        trace_demo.Variant("baseline", None),
        payload=ROOT / "examples/workloads/a8-l2.mlir",
        output_root=args.output_dir,
        tool_dir=args.llvm_root / "bin",
        plugin=plugin,
        runtime=runtime,
        expected_result=4096,
        target_func="a8_l2_kernel",
    )
    if trace["total_events"] != 4096:
        raise RuntimeError(f"Expected 4096 kernel reads, got {trace['total_events']}")
    output = args.output_dir / "simulation"
    product.run([runner, "--trace", args.output_dir / "baseline/trace.json",
                 "--output-dir", output, "--cache-profile", "amd-a8-3850-lru"])
    statistics = json.loads((output / "statistics.json").read_text(encoding="utf-8"))
    l1, l2 = (level["totals"] for level in statistics["levels"])
    if (l1["misses"], l1["evictions"], l2["hits"], l2["misses"]) != (4096, 3072, 2048, 2048):
        raise RuntimeError(f"Unexpected A8 cache statistics: L1={l1}, L2={l2}")
    print(f"A8 L2 workload passed: L1 misses={l1['misses']}, evictions={l1['evictions']}; "
          f"L2 hits={l2['hits']}, misses={l2['misses']}")
    print(f"Artifacts: {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
