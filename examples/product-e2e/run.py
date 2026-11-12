#!/usr/bin/env python3
"""Run the complete CacheScope compiler -> simulator -> UI product flow."""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VARIANTS = ("baseline", "tile4", "tile4-ikj")


def pinned_llvm_root() -> Path:
    version_file = ROOT / "cmake/LLVMVersion.cmake"
    match = re.search(
        r'^set\(CACHESCOPE_LLVM_MAJOR_VERSION "([0-9]+)"\)$',
        version_file.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if not match:
        raise RuntimeError(f"Cannot read LLVM major version from {version_file}")
    return Path(f"/usr/lib/llvm-{match.group(1)}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build CacheScope, produce three MLIR traces, replay them through "
            "an L1 cache and generate a standalone UI report."
        )
    )
    parser.add_argument(
        "--llvm-root",
        type=Path,
        default=Path(os.environ.get("LLVM_ROOT", pinned_llvm_root())),
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path(os.environ.get("BUILD_DIR", ROOT / "build")),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(
            os.environ.get("CACHESCOPE_OUTPUT_DIR", ROOT / "output")
        )
        / "product-e2e",
    )
    parser.add_argument(
        "--cache-profile",
        choices=("intel-486dx-geometry", "amd-a8-3850-lru"),
        help="Built-in approximate cache profile; cannot be combined with cache options.",
    )
    parser.add_argument(
        "--cache-size-bytes",
        type=int,
        default=None,
        help="Demonstration L1 capacity (default: 512).",
    )
    parser.add_argument(
        "--line-size-bytes",
        type=int,
        default=None,
        help="Cache line size (default: 64).",
    )
    parser.add_argument(
        "--associativity",
        type=int,
        default=None,
        help="Cache associativity (default: 1).",
    )
    parser.add_argument(
        "--replacement-policy",
        choices=("lru", "fifo", "random"),
        default=None,
        help="Replacement policy (default: lru).",
    )
    parser.add_argument(
        "--random-seed",
        type=int,
        default=None,
        help="Deterministic seed for random policy (default: 42).",
    )
    parser.add_argument(
        "--cache-level",
        action="append",
        dest="cache_levels",
        metavar="NAME:SIZE:LINE:WAYS:POLICY[:SEED]",
        help=(
            "Repeatable hierarchy level. When present, replaces the "
            "single-level cache options."
        ),
    )
    parser.add_argument(
        "--npm-ci",
        action="store_true",
        help="Reinstall locked UI dependencies even when node_modules exists.",
    )
    parser.add_argument(
        "--skip-ui",
        action="store_true",
        help="Produce traces and simulation reports without building HTML.",
    )
    args = parser.parse_args()
    cache_defaults = {
        "cache_size_bytes": 512,
        "line_size_bytes": 64,
        "associativity": 1,
        "replacement_policy": "lru",
        "random_seed": 42,
    }
    if args.cache_profile and (
        args.cache_levels or any(getattr(args, name) is not None for name in cache_defaults)
    ):
        parser.error("--cache-profile cannot be combined with cache configuration options")
    for name, default in cache_defaults.items():
        if getattr(args, name) is None:
            setattr(args, name, default)
    return args


def run(
    command: list[object], *, cwd: Path = ROOT, capture: bool = False
) -> subprocess.CompletedProcess[str]:
    rendered = [str(argument) for argument in command]
    print("+", shlex.join(rendered), flush=True)
    return subprocess.run(
        rendered,
        cwd=cwd,
        check=True,
        text=True,
        capture_output=capture,
    )


def require_file(path: Path, description: str) -> Path:
    if not path.is_file():
        raise RuntimeError(f"Missing {description}: {path}")
    return path


def find_library(directory: Path, stem: str) -> Path:
    for suffix in (".so", ".dylib", ".dll"):
        candidate = directory / f"{stem}{suffix}"
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"Missing built library: {directory / (stem + '.so')}")


def configure_and_build(args: argparse.Namespace) -> tuple[Path, Path, Path]:
    run(
        [
            "cmake",
            "-S",
            ROOT,
            "-B",
            args.build_dir,
            "-G",
            "Ninja",
            f"-DMLIR_DIR={args.llvm_root / 'lib/cmake/mlir'}",
            f"-DCACHESCOPE_CLANG_TOOLS_DIR={args.llvm_root / 'bin'}",
            f"-DCMAKE_C_COMPILER={args.llvm_root / 'bin/clang'}",
            f"-DCMAKE_CXX_COMPILER={args.llvm_root / 'bin/clang++'}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCACHESCOPE_BUILD_CACHE_MODEL=ON",
            "-DBUILD_TESTING=ON",
        ]
    )
    run(
        [
            "cmake",
            "--build",
            args.build_dir,
            "--target",
            "CacheScopePassPlugin",
            "cachescope_trace_runtime",
            "cachescope_runner_cli",
        ]
    )
    plugin = find_library(args.build_dir / "lib", "CacheScopePassPlugin")
    runtime = find_library(
        args.build_dir / "compiler/runtime", "libcachescope_trace_runtime"
    )
    runner = require_file(
        args.build_dir / "runner/cachescope-runner", "CacheScope runner CLI"
    )
    return plugin, runtime, runner


def produce_traces(
    args: argparse.Namespace, plugin: Path, runtime: Path
) -> None:
    run(
        [
            "python3",
            ROOT / "examples/trace_demo.py",
            "--llvm-root",
            args.llvm_root,
            "--build-dir",
            args.build_dir,
            "--output-dir",
            args.output_dir,
            "--plugin",
            plugin,
            "--runtime",
            runtime,
        ]
    )


def read_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as source:
        value = json.load(source)
    if not isinstance(value, dict):
        raise RuntimeError(f"Expected a JSON object: {path}")
    return value


def replay_variants(args: argparse.Namespace, runner: Path) -> list[dict]:
    results: list[dict] = []
    for variant in VARIANTS:
        trace_path = args.output_dir / variant / "trace.json"
        simulation_dir = args.output_dir / "simulation" / f"{variant}-l1"
        command: list[object] = [
            runner,
            "--trace",
            trace_path,
            "--output-dir",
            simulation_dir,
        ]
        if args.cache_profile:
            command.extend(["--cache-profile", args.cache_profile])
        elif args.cache_levels:
            for level in args.cache_levels:
                command.extend(["--cache-level", level])
        else:
            command.extend(
                [
                    "--cache-name",
                    "L1",
                    "--size-bytes",
                    args.cache_size_bytes,
                    "--line-size-bytes",
                    args.line_size_bytes,
                    "--associativity",
                    args.associativity,
                    "--replacement-policy",
                    args.replacement_policy,
                    "--random-seed",
                    args.random_seed,
                ]
            )
        run(command)

        trace = read_json(trace_path)
        statistics_path = simulation_dir / "statistics.json"
        events_path = simulation_dir / "events.json"
        statistics = read_json(statistics_path)
        events = read_json(events_path)
        if statistics.get("schema") != "cachescope.simulation-statistics.v1":
            raise RuntimeError(f"Unexpected statistics schema: {statistics_path}")
        if events.get("schema") != "cachescope.simulation-events.v1":
            raise RuntimeError(f"Unexpected events schema: {events_path}")
        if statistics.get("variant") != variant or events.get("variant") != variant:
            raise RuntimeError(f"Variant mismatch in simulation output: {variant}")
        if statistics.get("processed", {}).get("trace_events") != trace.get(
            "recorded_events"
        ):
            raise RuntimeError(f"Trace/statistics event mismatch: {variant}")
        if len(events.get("events", [])) != trace.get("recorded_events"):
            raise RuntimeError(f"Trace/timeline event mismatch: {variant}")
        results.append(statistics)
    if not args.cache_profile and not args.cache_levels and (
        args.cache_size_bytes,
        args.line_size_bytes,
        args.associativity,
        args.replacement_policy,
    ) == (512, 64, 1, "lru"):
        misses = [item["totals"]["misses"] for item in results]
        if not misses[0] > misses[1] > misses[2]:
            raise RuntimeError(
                "Default demonstration cache must distinguish all variants "
                "and improve baseline -> tile4 -> tile4-ikj"
            )
    return results


def node_is_supported() -> bool:
    if shutil.which("node") is None or shutil.which("npm") is None:
        return False
    version = run(["node", "--version"], capture=True).stdout.strip()
    match = re.fullmatch(r"v([0-9]+)\.([0-9]+)\.[0-9]+", version)
    if not match:
        return False
    major, minor = (int(value) for value in match.groups())
    return (major == 20 and minor >= 19) or (major == 22 and minor >= 12) or major > 22


def build_ui_report(args: argparse.Namespace) -> Path:
    if not node_is_supported():
        raise RuntimeError("UI requires Node.js ^20.19.0 or >=22.12.0 and npm")
    ui_dir = ROOT / "ui"
    if args.npm_ci or not (ui_dir / "node_modules/.bin/vite").is_file():
        run(["npm", "ci"], cwd=ui_dir)
    report = args.output_dir / "report.html"
    run(
        [
            "npm",
            "run",
            "report",
            "--",
            "--input",
            args.output_dir,
            "--output",
            report,
        ],
        cwd=ui_dir,
    )
    require_file(report, "standalone UI report")
    if report.stat().st_size == 0:
        raise RuntimeError(f"Empty standalone UI report: {report}")
    return report


def publish_results(
    args: argparse.Namespace,
    statistics: list[dict],
    report: Path | None,
) -> None:
    def format_rate(value: float | None) -> str:
        return "n/a" if value is None else f"{value:.2f}%"

    levels = statistics[0]["levels"]
    configurations = [level["cache_config"] for level in levels]
    for item in statistics[1:]:
        if [level["cache_config"] for level in item["levels"]] != configurations:
            raise RuntimeError("Cache hierarchy differs between variants")

    hierarchy = " -> ".join(
        f"{level['name']} {config['size_bytes']} B, "
        f"line {config['line_size_bytes']} B, "
        f"{config['associativity']}-way {config['replacement_policy']}"
        for level, config in zip(levels, configurations, strict=True)
    )
    print("\nCacheScope product E2E passed")
    print(f"Hierarchy: {hierarchy}")
    baseline = statistics[0]["totals"]
    baseline_hit_rate = baseline["hits"] / (
        baseline["hits"] + baseline["misses"]
    )
    rows: list[tuple[str, int, int, float, int, float]] = []
    for item in statistics:
        totals = item["totals"]
        hit_rate = totals["hits"] / (totals["hits"] + totals["misses"])
        miss_delta = totals["misses"] - baseline["misses"]
        hit_rate_delta = (hit_rate - baseline_hit_rate) * 100
        rows.append(
            (
                item["variant"],
                totals["hits"],
                totals["misses"],
                hit_rate * 100,
                miss_delta,
                hit_rate_delta,
            )
        )
        print(
            f"{item['variant']:12} hits={totals['hits']:6} "
            f"misses={totals['misses']:6} "
            f"hit-rate={hit_rate * 100:6.2f}% "
            f"delta-misses={miss_delta:+6} "
            f"delta-hit-rate={hit_rate_delta:+6.2f} pp"
        )
        for level in item["levels"][1:]:
            level_totals = level["totals"]
            print(
                f"{'':12} {level['name']:4} hits={level_totals['hits']:6} "
                f"misses={level_totals['misses']:6} "
                f"hit-rate={format_rate(level_totals['hit_rate_percent']):>7}"
            )
        print(
            f"{'':12} memory reads={item['memory']['reads']:6} "
            f"writebacks={item['memory']['writebacks']:6}"
        )
    print(f"Experiment: {args.output_dir}")
    if report is not None:
        print(f"UI report:  {report}")

    best_misses = min(row[2] for row in rows)
    best = [row for row in rows if row[2] == best_misses]
    result_lines = [
        "# Результаты CacheScope product E2E",
        "",
        "Иерархия: exclusive, write-back, write-allocate.",
        "",
    ]
    if args.cache_profile:
        profile = statistics[0]["profile"]
        result_lines.extend([
            f"Профиль: `{profile['id']}` — приближение аппаратной модели.",
            f"Источник: {profile['source']}",
            *[f"- {item}" for item in profile["limitations"]],
            "",
        ])
    result_lines.extend([
        "| Level | Capacity | Line | Ways | Policy |",
        "|---|---:|---:|---:|---|",
    ])
    for level, config in zip(levels, configurations, strict=True):
        result_lines.append(
            f"| `{level['name']}` | {config['size_bytes']} B | "
            f"{config['line_size_bytes']} B | {config['associativity']} | "
            f"`{config['replacement_policy']}` |"
        )
    result_lines.extend(
        [
        "",
        "## Сравнение L1",
        "",
        "| Variant | Hits | Misses | Hit rate | Δ misses к baseline | Δ hit rate |",
        "|---|---:|---:|---:|---:|---:|",
        ]
    )
    for variant, hits, misses, hit_rate, miss_delta, hit_rate_delta in rows:
        result_lines.append(
            f"| `{variant}` | {hits} | {misses} | {hit_rate:.2f}% | "
            f"{miss_delta:+d} | {hit_rate_delta:+.2f} pp |"
        )
    if len(levels) == 1:
        if len(best) == 1:
            conclusion = (
                f"Минимум промахов: `{best[0][0]}` — {best[0][2]} "
                f"({best[0][4]:+d} относительно baseline)."
            )
        else:
            variants = ", ".join(f"`{row[0]}`" for row in best)
            conclusion = (
                f"Одинаковый минимум {best_misses} у вариантов {variants}; "
                "эта модель кеша не различает их по misses."
            )
    else:
        result_lines.extend(["", "## Статистика нижних уровней"])
        for index, level in enumerate(levels[1:], start=1):
            result_lines.extend(
                [
                    "",
                    f"### {level['name']}",
                    "",
                    "| Variant | Hits | Misses | Hit rate |",
                    "|---|---:|---:|---:|",
                ]
            )
            for item in statistics:
                totals = item["levels"][index]["totals"]
                result_lines.append(
                    f"| `{item['variant']}` | {totals['hits']} | "
                    f"{totals['misses']} | "
                    f"{format_rate(totals['hit_rate_percent'])} |"
                )
        result_lines.extend(
            [
                "",
                "## Обращения к памяти",
                "",
                "| Variant | Reads | Writebacks |",
                "|---|---:|---:|",
            ]
        )
        for item in statistics:
            result_lines.append(
                f"| `{item['variant']}` | {item['memory']['reads']} | "
                f"{item['memory']['writebacks']} |"
            )
        best_memory_reads = min(item["memory"]["reads"] for item in statistics)
        best_memory = [
            item["variant"]
            for item in statistics
            if item["memory"]["reads"] == best_memory_reads
        ]
        variants = ", ".join(f"`{variant}`" for variant in best_memory)
        conclusion = (
            f"Минимум чтений из памяти: {best_memory_reads} у {variants}. "
            "Промах уровня не означает обращение к памяти, если строка найдена "
            "на следующем уровне."
        )
    result_lines.extend(
        [
            "",
            conclusion,
            "Hits и misses следует сравнивать отдельно для каждого уровня.",
        ]
    )
    if report is not None:
        result_lines.extend(["", "Автономный UI: [report.html](report.html)."])
    results_path = args.output_dir / "RESULTS.md"
    results_path.write_text("\n".join(result_lines) + "\n", encoding="utf-8")
    print(f"Summary:    {results_path}")


def main() -> int:
    args = parse_args()
    for name in ("cache_size_bytes", "line_size_bytes", "associativity"):
        if getattr(args, name) <= 0:
            raise RuntimeError(f"--{name.replace('_', '-')} must be positive")
    if args.random_seed < 0 or args.random_seed > 2**64 - 1:
        raise RuntimeError("--random-seed must fit an unsigned 64-bit integer")
    args.llvm_root = args.llvm_root.resolve()
    args.build_dir = args.build_dir.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    plugin, runtime, runner = configure_and_build(args)
    produce_traces(args, plugin, runtime)
    statistics = replay_variants(args, runner)
    report = None if args.skip_ui else build_ui_report(args)
    publish_results(args, statistics, report)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Product E2E failed: {error}") from error
