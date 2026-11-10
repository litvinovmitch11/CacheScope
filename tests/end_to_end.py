#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from common import run


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--mlir-bin", type=Path, required=True)
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    with tempfile.TemporaryDirectory(prefix="cachescope-e2e-") as directory:
        output = Path(directory)
        result = run(
            [
                sys.executable,
                args.repo / "examples/trace_demo.py",
                "--llvm-build-dir",
                args.mlir_bin.parent,
                "--output-dir",
                output,
                "--plugin",
                args.plugin,
                "--runtime",
                args.runtime,
            ],
            cwd=args.repo,
        )
        summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
        assert set(summary["variants"]) == {"baseline", "tile4", "tile4-ikj"}
        assert len(summary["comparisons"]) == 3
        assert all(
            variant["valid"] and variant["complete"]
            for variant in summary["variants"].values()
        )

    final_line = result.stdout.strip().splitlines()[-1]
    print(f"end-to-end suite OK; {final_line}")


if __name__ == "__main__":
    main()
