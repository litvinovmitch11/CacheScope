#!/usr/bin/env python3
"""Run a trusted local Torch + MLIR Transform project through CacheScope."""

from __future__ import annotations

import copy
import ctypes
import importlib.util
import json
import os
import re
import shutil
import sys
from pathlib import Path

import numpy as np
import torch
from torch_mlir.fx import export_and_import
from torch_mlir.runtime import get_ranked_memref_descriptor


ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "examples/ml-locality/depthwise-temporal-blocking"
sys.path.insert(0, str(DEMO))
sys.path.insert(0, str(ROOT / "scripts"))

import run as pipeline  # noqa: E402
from validate_trace import validate_trace  # noqa: E402

# The reusable demo pipeline imports its own ``model`` module. Remove only that
# generic name so a local project's inputs.py can import its neighboring model.py.
sys.modules.pop("model", None)


def write_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def progress(job: Path, stage: str, message: str, **extra: object) -> None:
    write_json(job / "progress.json", {"stage": stage, "message": message, **extra})


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path.name}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def static_f32_tensor(value: object, label: str) -> torch.Tensor:
    if not isinstance(value, torch.Tensor):
        raise TypeError(f"{label} must be a torch.Tensor")
    if value.dtype != torch.float32 or value.device.type != "cpu":
        raise TypeError(f"{label} must be a CPU float32 tensor")
    if value.ndim == 0 or any(not isinstance(size, int) or size <= 0 for size in value.shape):
        raise ValueError(f"{label} must have a non-empty static shape")
    return value.detach().contiguous()


def mlir_memref(tensor: torch.Tensor) -> str:
    return "memref<" + "x".join(str(size) for size in tensor.shape) + "xf32>"


def native_wrapper(inputs: tuple[torch.Tensor, ...], output: torch.Tensor):
    input_types = [mlir_memref(item) for item in inputs]
    output_type = mlir_memref(output)

    def insert(module_path: Path, output_path: Path) -> None:
        text = module_path.read_text(encoding="utf-8")
        module_start = text.find("module ")
        located_end = re.search(r"^} loc\(#loc[0-9]+\)$", text[module_start:], re.MULTILINE)
        if located_end is not None:
            closing = module_start + located_end.start()
        else:
            plain_end = text.find("\n}\n", module_start)
            closing = plain_end + 1 if plain_end >= 0 else -1
        if closing < 0:
            raise RuntimeError("cannot find the exported MLIR module boundary")

        arguments = [f"%arg{index}: {kind}" for index, kind in enumerate(input_types)]
        arguments.append(f"%output: {output_type}")
        call_args = ", ".join(f"%arg{index}" for index in range(len(input_types)))
        call_types = ", ".join(input_types)
        lines = [
            "",
            "  func.func @cachescope_entry(",
            "      " + ", ".join(arguments) + ")",
            "      attributes {llvm.emit_c_interface} {",
            f"    %result = func.call @forward({call_args}) : ({call_types}) -> {output_type}",
            "    %c0 = arith.constant 0 : index",
            "    %c1 = arith.constant 1 : index",
        ]
        indices = []
        for rank, size in enumerate(output.shape):
            lines.append(f"    %c{rank}_size = arith.constant {size} : index")
            indent = "    " + "  " * rank
            lines.append(
                f"{indent}scf.for %i{rank} = %c0 to %c{rank}_size step %c1 {{"
            )
            indices.append(f"%i{rank}")
        suffix = ", ".join(indices)
        body_indent = "    " + "  " * len(output.shape)
        lines.extend(
            [
                f"{body_indent}%value = memref.load %result[{suffix}] : {output_type}",
                f"{body_indent}memref.store %value, %output[{suffix}] : {output_type}",
            ]
        )
        for rank in reversed(range(len(output.shape))):
            lines.append("    " + "  " * rank + "}")
        lines.extend(
            [
                f"    memref.dealloc %result : {output_type}",
                "    return",
                "  }",
                "",
            ]
        )
        output_path.write_text(text[:closing] + "\n".join(lines) + text[closing:], encoding="utf-8")

    return insert


def execute_variant(
    variant: pipeline.Variant,
    artifacts: dict[str, Path],
    inputs: tuple[torch.Tensor, ...],
    output_shape: tuple[int, ...],
    trace_limit: int,
) -> tuple[torch.Tensor, dict]:
    arrays = [np.ascontiguousarray(item.numpy(), dtype=np.float32) for item in inputs]
    output = np.empty(output_shape, dtype=np.float32)
    descriptors = [get_ranked_memref_descriptor(array) for array in arrays]
    output_descriptor = get_ranked_memref_descriptor(output)
    library = ctypes.CDLL(str(artifacts["library"]), mode=os.RTLD_LOCAL)
    entry = library._mlir_ciface_cachescope_entry
    entry.argtypes = [
        *(ctypes.POINTER(type(item)) for item in descriptors),
        ctypes.POINTER(type(output_descriptor)),
    ]
    entry.restype = None
    reset = library.__cachescope_trace_reset_v1
    flush = library.__cachescope_trace_flush_v1
    reset.argtypes = flush.argtypes = []
    reset.restype = flush.restype = None
    os.environ.update(
        CACHESCOPE_TRACE_FILE=str(artifacts["trace"]),
        CACHESCOPE_VARIANT=variant.name,
        CACHESCOPE_PIPELINE_FINGERPRINT=f"local-project-{variant.name}-v1",
        CACHESCOPE_TRACE_LIMIT=str(trace_limit),
    )
    reset()
    entry(
        *(ctypes.byref(item) for item in descriptors),
        ctypes.byref(output_descriptor),
    )
    flush()
    trace = json.loads(artifacts["trace"].read_text(encoding="utf-8"))
    validate_trace(trace, artifact_root=artifacts["directory"])
    if not trace["valid"] or not trace["complete"] or trace["truncated"]:
        raise RuntimeError("trace is incomplete; increase trace_limit")
    if any(event["address_kind"] != "memref_offset" for event in trace["events"]):
        raise RuntimeError("execution produced non-semantic memory addresses")
    return torch.from_numpy(output.copy()), trace


def build_stepper(
    output: Path, variant: pipeline.Variant, trace: dict, event_limit: int
) -> dict | None:
    if event_limit == 0:
        return None
    preview = copy.deepcopy(trace)
    count = min(event_limit, len(preview["events"]))
    preview["events"] = preview["events"][:count]
    preview["total_events"] = preview["recorded_events"] = count
    preview["complete"], preview["truncated"] = True, False
    preview["producer"]["derived_from_variant"] = variant.name
    preview["producer"]["source_total_events"] = trace["total_events"]
    bundle = output / "stepper" / variant.name
    shutil.rmtree(bundle, ignore_errors=True)
    bundle.mkdir(parents=True)
    write_json(bundle / "trace.json", preview)
    for stage in preview["source_map"]["stages"]:
        relative = Path(stage["path"])
        destination = bundle / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(output / variant.name / relative, destination)
    validate_trace(preview, artifact_root=bundle)
    return {"variant": variant.name, "events": count}


def replay_profile(
    runner: Path,
    trace: Path,
    destination: Path,
    profile: object,
) -> dict:
    if isinstance(profile, list):
        return pipeline.replay(
            runner, trace, destination, tuple(profile), keep_events=False
        )
    if not isinstance(profile, dict) or not isinstance(profile.get("levels"), list):
        raise TypeError("cache profile must be compact levels or {levels, hierarchy}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    config = destination.with_name(destination.name + ".config.json")
    write_json(config, profile)
    pipeline.run(
        [
            runner,
            "--trace",
            trace,
            "--output-dir",
            destination,
            "--config",
            config,
        ]
    )
    statistics = json.loads(
        (destination / "statistics.json").read_text(encoding="utf-8")
    )
    (destination / "events.json").unlink(missing_ok=True)
    return statistics


def run(job: Path) -> None:
    project = json.loads((job / "project.json").read_text(encoding="utf-8"))
    workspace, output = job / "project", job / "output"
    output.mkdir()
    sys.path.insert(0, str(workspace))

    progress(job, "preparation", "Loading the local Torch project")
    model_module = load_module(workspace / "model.py", "cachescope_user_model")
    inputs_module = load_module(workspace / "inputs.py", "cachescope_user_inputs")
    model = model_module.create_model()
    if not isinstance(model, torch.nn.Module):
        raise TypeError("create_model() must return torch.nn.Module")
    created_inputs = inputs_module.create_inputs()
    if not isinstance(created_inputs, (tuple, list)) or not created_inputs:
        raise TypeError("create_inputs() must return a non-empty tuple of tensors")
    inputs = tuple(static_f32_tensor(item, f"input {index}") for index, item in enumerate(created_inputs))
    model.eval()
    with torch.no_grad():
        expected = static_f32_tensor(model(*inputs), "model output")

    variants = tuple(
        pipeline.Variant(item["name"], workspace / item["path"], None)
        for item in project["variants"]
    )
    progress(job, "export", "Exporting Torch to Linalg-on-tensors MLIR")
    module = export_and_import(model, *inputs, output_type="linalg-on-tensors", func_name="forward")
    payload = output / "00-pytorch-linalg.mlir"
    payload.write_text(str(module), encoding="utf-8")

    class Args:
        pass

    args = Args()
    args.output_dir = output
    args.build_dir = Path(project["runtime"]["build_dir"])
    args.llvm_root = Path(project["runtime"]["llvm_root"])
    args.skip_build = True
    args.keep_events = False
    args.stepper_events = project["settings"]["stepper_events"]
    args.progress_file = job / "progress.json"
    pipeline.add_native_entry = native_wrapper(inputs, expected)
    progress(job, "toolchain", "Checking the local CacheScope toolchain")
    plugin, runtime, runner = pipeline.configure_and_build(args)

    traces: dict[str, dict] = {}
    successful: list[pipeline.Variant] = []
    results: dict[str, dict] = {}
    reference_trace: dict | None = None
    tolerance = project["settings"]["validation"]
    for index, variant in enumerate(variants, 1):
        progress(job, "compilation", f"Compiling {variant.name}", current=index, total=len(variants), variant=variant.name)
        try:
            artifacts = pipeline.compile_variant(args, variant, payload, plugin, runtime)
            actual, trace = execute_variant(
                variant, artifacts, inputs, tuple(expected.shape), project["settings"]["trace_limit"]
            )
            torch.testing.assert_close(actual, expected, rtol=tolerance["rtol"], atol=tolerance["atol"])
            if reference_trace is None:
                reference_trace = trace
            elif (
                trace["source_map"]["payload_fingerprint"]
                != reference_trace["source_map"]["payload_fingerprint"]
                or trace["source_map"]["origins"]
                != reference_trace["source_map"]["origins"]
            ):
                raise RuntimeError("schedule lost comparable high-level provenance")
            traces[variant.name] = trace
            successful.append(variant)
            results[variant.name] = {
                "status": "completed",
                "validation": {
                    "torch_reference": "passed",
                    "semantic_provenance": "passed",
                    **tolerance,
                },
                "trace_metrics": pipeline.trace_metrics(trace),
                "cache_profiles": {},
            }
        except Exception as error:
            diagnostic = str(error)
            stderr = getattr(error, "stderr", None)
            if stderr:
                diagnostic += "\n" + str(stderr)[-12000:]
            results[variant.name] = {"status": "failed", "error": diagnostic, "cache_profiles": {}}
    if not successful:
        raise RuntimeError("all schedules failed; inspect the per-schedule diagnostics")

    total = len(successful) * len(project["cache_profiles"])
    current = 0
    simulations: dict[str, dict[str, dict]] = {}
    for profile, configuration in project["cache_profiles"].items():
        simulations[profile] = {}
        for variant in successful:
            current += 1
            progress(job, "simulation", f"Simulating {variant.name} / {profile}", current=current, total=total, variant=variant.name)
            statistics = replay_profile(
                runner, output / variant.name / "trace.json",
                output / "simulation" / f"{profile}--{variant.name}",
                configuration,
            )
            simulations[profile][variant.name] = statistics
            results[variant.name]["cache_profiles"][profile] = pipeline.simulation_metrics(statistics)

    progress(job, "stepper", "Building Stepper bundles for successful schedules")
    stepper_variants = {
        variant.name: bundle
        for variant in successful
        if (bundle := build_stepper(
            output, variant, traces[variant.name], args.stepper_events
        )) is not None
    }
    preferred = next((item for item in successful if item.name != "baseline"), successful[0])
    first_profile = next(iter(project["cache_profiles"]))
    def stepper_profile(statistics: dict) -> dict:
        hierarchy = statistics["hierarchy"]
        return {
            "levels": statistics["levels"],
            "hierarchy": {
                "inclusion_policies": hierarchy["inclusion_policies"],
                "writeback_clean": hierarchy["writeback_clean"],
                "non_inclusive_fill": hierarchy["non_inclusive_fill"],
            },
        }

    summary = {
        "schema": "cachescope.torch-project-result.v1",
        "project": project["name"],
        "input_shapes": [list(item.shape) for item in inputs],
        "output_shape": list(expected.shape),
        "stepper_variants": stepper_variants,
        # Kept for readers of the first result schema. New UI always asks the
        # user which successful schedule to inspect.
        "stepper_demo": stepper_variants.get(preferred.name),
        "stepper_levels": simulations[first_profile][preferred.name]["levels"],
        "stepper_profiles": {
            profile: stepper_profile(simulations[profile][preferred.name])
            for profile in project["cache_profiles"]
        },
        "variants": results,
    }
    write_json(output / "summary.json", summary)
    entries = []
    trace_fields = ("schema", "variant", "total_events", "recorded_events", "truncated", "valid", "complete")
    for variant in successful:
        entries.append({"path": f"{variant.name}/trace.json", "data": {key: traces[variant.name][key] for key in trace_fields}})
        for profile in project["cache_profiles"]:
            entries.append({"path": f"simulation/{profile}--{variant.name}/statistics.json", "data": simulations[profile][variant.name]})
    write_json(
        output / "ui-results.json",
        {
            "entries": entries,
            "variants": results,
            "contract": {
                "input_shapes": [list(item.shape) for item in inputs],
                "output_shape": list(expected.shape),
                "validation": tolerance,
            },
        },
    )
    progress(job, "publishing", "Experiment artifacts are ready")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: torch_project_worker.py JOB_DIRECTORY")
    job = Path(sys.argv[1]).resolve()
    try:
        run(job)
    except Exception as error:
        stage = "execution"
        try:
            stage = json.loads((job / "progress.json").read_text())["stage"]
        except Exception:
            pass
        write_json(job / "error.json", {"code": "project_failed", "message": str(error), "stage": stage})
        print(f"project failed: {error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
