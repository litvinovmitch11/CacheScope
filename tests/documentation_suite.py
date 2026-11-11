#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path
from urllib.parse import unquote


LINK_PATTERN = re.compile(r"\[[^\]]+\]\(([^)]+)\)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    return parser.parse_args()


def markdown_files(repo: Path) -> list[Path]:
    files = [repo / "README.md", repo / "ROADMAP.md", repo / "CONTRIBUTING.md"]
    files.extend((repo / "docs").glob("*.md"))
    files.extend((repo / "examples").glob("**/README.md"))
    files.append(repo / "compiler/cs-trace/ARCHITECTURE.md")
    files.append(repo / "runner/README.md")
    files.append(repo / "simulator/README.md")
    files.append(repo / "schemas/README.md")
    files.append(repo / "ui/README.md")
    return sorted(set(files))


def main() -> None:
    args = parse_args()
    failures: list[str] = []
    required_files = (
        ".clang-format",
        ".clang-tidy",
        ".github/workflows/compiler-tests.yml",
        "cmake/LLVMVersion.cmake",
        "schemas/cachescope.trace.v1.schema.json",
        "docs/CACHE_MODELS.md",
        "examples/trace_demo.py",
        "examples/product-e2e/run.py",
        "schemas/fixtures/source-map-v1/trace.json",
        "scripts/install-llvm-dependencies.sh",
        "scripts/validate_trace.py",
    )
    for relative_path in required_files:
        if not (args.repo / relative_path).is_file():
            failures.append(f"missing repository contract: {relative_path}")

    if (args.repo / ".gitmodules").exists():
        failures.append("obsolete repository contract: .gitmodules")

    compiler_workflow = (
        args.repo / ".github/workflows/compiler-tests.yml"
    ).read_text(encoding="utf-8")
    for obsolete_ci_fragment in (
        "actions/cache",
        "build-llvm.sh",
        ".deps/llvm",
    ):
        if obsolete_ci_fragment in compiler_workflow:
            failures.append(
                "compiler workflow must not build or cache LLVM: "
                f"{obsolete_ci_fragment}"
            )
    if "scripts/install-llvm-dependencies.sh" not in compiler_workflow:
        failures.append("compiler workflow does not install pinned LLVM packages")

    for document in markdown_files(args.repo):
        if not document.is_file():
            failures.append(f"missing document: {document.relative_to(args.repo)}")
            continue
        text = document.read_text(encoding="utf-8")
        for obsolete_command in ("git submodule", "--recurse-submodules"):
            if obsolete_command in text:
                failures.append(
                    f"{document.relative_to(args.repo)}: obsolete command "
                    f"{obsolete_command}"
                )
        for match in LINK_PATTERN.finditer(text):
            raw_target = match.group(1).strip().strip("<>")
            if raw_target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            target_without_anchor = unquote(raw_target.split("#", 1)[0])
            target = (document.parent / target_without_anchor).resolve()
            if not target.exists():
                failures.append(
                    f"{document.relative_to(args.repo)}: broken link {raw_target}"
                )

    if failures:
        raise SystemExit("\n".join(failures))
    print("documentation suite OK")


if __name__ == "__main__":
    main()
