#!/usr/bin/env python3
"""Isolated compiler/input worker for the local experiment API."""
from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import resource
from pathlib import Path
import subprocess
import sys
import zipfile

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('CACHESCOPE_BUILD_DIR', ROOT / 'build/app'))
LLVM = Path(os.environ.get('LLVM_ROOT', '/usr/lib/llvm-22'))
PROGRAM = BUILD / 'bin/cachescope-program'
RUNTIME = BUILD / 'compiler/runtime/libcachescope_trace_runtime.so'
PLUGIN = BUILD / 'lib/CacheScopePassPlugin.so'


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value), encoding='utf-8')
    temporary.replace(path)


def stage(job, name):
    write_json(job / 'progress.json', dict(stage=name))


def command(*args):
    print('+', *(str(arg) for arg in args), flush=True)
    subprocess.run([str(arg) for arg in args], check=True)


def inspect_arrays(path):
    arrays = []
    with zipfile.ZipFile(path) as archive:
        seen = set()
        for entry in archive.infolist():
            if not entry.filename.endswith('.npy') or entry.filename in seen:
                raise ValueError('NPZ must contain uniquely named NPY arrays')
            seen.add(entry.filename)
            with archive.open(entry) as array:
                version = np.lib.format.read_magic(array)
                if version == (1, 0):
                    shape, _, dtype = np.lib.format.read_array_header_1_0(array)
                elif version == (2, 0):
                    shape, _, dtype = np.lib.format.read_array_header_2_0(array)
                else:
                    raise ValueError('Unsupported NPY format version')
                if dtype.hasobject:
                    raise ValueError('Object/pickle arrays are forbidden')
                if math.prod(shape) * dtype.itemsize != entry.file_size - array.tell():
                    raise ValueError('NPZ array payload does not match its shape')
                arrays.append(dict(key=entry.filename[:-4], shape=list(shape), dtype=str(dtype)))
    return arrays


def prepare(job):
    stage(job, 'inspection')
    result = subprocess.run([str(PROGRAM), 'inspect', str(job / 'input.mlir')],
                            text=True, stdout=subprocess.PIPE, check=True)
    functions = json.loads(result.stdout)
    if not any(function['supported'] for function in functions):
        reasons = '; '.join(f"{f['name']}: {f['reason']}" for f in functions)
        raise ValueError('No supported functions. ' + reasons)
    write_json(job / 'metadata.json', dict(functions=functions, arrays=inspect_arrays(job / 'inputs.npz')))


def descriptor(array):
    rank = array.ndim
    pointer = ctypes.POINTER(ctypes.c_float)
    class Memref(ctypes.Structure):
        _fields_ = [('allocated', pointer), ('aligned', pointer), ('offset', ctypes.c_int64),
                    ('sizes', ctypes.c_int64 * rank), ('strides', ctypes.c_int64 * rank)]
    return Memref(array.ctypes.data_as(pointer), array.ctypes.data_as(pointer), 0,
                  (ctypes.c_int64 * rank)(*array.shape),
                  (ctypes.c_int64 * rank)(*(stride // array.itemsize for stride in array.strides)))


def invoke(job, spec, function):
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    arrays = []
    with np.load(job / 'inputs.npz', allow_pickle=False) as archive:
        for argument, binding in zip(function['arguments'], spec['bindings'], strict=True):
            shape = tuple(argument['shape'])
            if binding['source'] == 'zeros':
                array = np.zeros(shape, dtype=np.float32)
            else:
                source = archive[binding['key']]
                if source.dtype != np.dtype(np.float32) or source.shape != shape:
                    raise ValueError('Tensor dtype or shape does not match the function')
                array = np.array(source, dtype=np.float32, order='C', copy=True)
            arrays.append(array)
    os.environ.update(CACHESCOPE_TRACE_FILE=str(job / 'baseline/trace.json'),
                      CACHESCOPE_VARIANT='baseline',
                      CACHESCOPE_PIPELINE_FINGERPRINT='ui-baseline-v1',
                      CACHESCOPE_TRACE_LIMIT=spec['trace_limit'] or 'unlimited')
    library = ctypes.CDLL(str(job / 'baseline/program.so'), mode=os.RTLD_LOCAL)
    descriptions = [descriptor(array) for array in arrays]
    entry = library._mlir_ciface_cachescope_entry
    entry.argtypes = [ctypes.POINTER(type(item)) for item in descriptions]
    entry.restype = None
    reset = getattr(library, '__cachescope_trace_reset_v1')
    flush = getattr(library, '__cachescope_trace_flush_v1')
    reset.argtypes = flush.argtypes = []
    reset.restype = flush.restype = None
    reset()
    entry(*(ctypes.byref(item) for item in descriptions))
    flush()


def execute(job, spec, function):
    sys.path.insert(0, str(ROOT / 'examples'))
    from trace_demo import Variant, compile_stages
    stage(job, 'compilation')
    wrapped = job / 'wrapped.mlir'
    command(PROGRAM, 'wrap', job / 'input.mlir', spec['function'], wrapped)
    executable, trace = compile_stages(
        Variant('baseline', None), payload=wrapped, output_root=job,
        tool_dir=LLVM / 'bin', plugin=PLUGIN,
        target_function='__cachescope_kernel', lower_affine=True,
    )
    directory = executable.parent
    command(LLVM / 'bin/mlir-translate', executable, '--mlir-to-llvmir', '-o', directory / 'module.ll')
    command(LLVM / 'bin/llc', '-filetype=obj', '-relocation-model=pic', directory / 'module.ll', '-o', directory / 'module.o')
    command(LLVM / 'bin/clang++', '-shared', directory / 'module.o', RUNTIME,
            '-Wl,-rpath,' + str(RUNTIME.parent), '-o', directory / 'program.so')
    stage(job, 'execution')
    command(sys.executable, __file__, 'invoke', '--job', job)
    sys.path.insert(0, str(ROOT / 'scripts'))
    from validate_trace import validate_trace
    value = json.loads(trace.read_text())
    validate_trace(value, artifact_root=directory)
    if value['truncated']:
        raise ValueError('trace_limit_exceeded: requested event limit was exceeded')
    if not value['valid'] or not value['complete']:
        raise ValueError('Execution produced an invalid or incomplete trace')
    if any(event['address_kind'] != 'memref_offset' for event in value['events']):
        raise ValueError('Execution produced unsupported process-address accesses')
    header = {key: value[key] for key in ['schema', 'variant', 'total_events', 'recorded_events', 'valid', 'complete', 'truncated']}
    del value
    stage(job, 'simulation')
    args = [BUILD / 'runner/cachescope-runner', '--trace', trace, '--output-dir', job / 'simulation/baseline',
            '--config', job / 'spec.json']
    command(*args)
    statistics = json.loads((job / 'simulation/baseline/statistics.json').read_text())
    write_json(job / 'report.json', dict(statistics=statistics, trace=header))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['prepare', 'run', 'invoke'])
    parser.add_argument('--job', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.mode == 'prepare':
            prepare(args.job)
        else:
            spec = json.loads((args.job / 'spec.json').read_text())
            metadata = json.loads((args.job / 'metadata.json').read_text())
            function = next(f for f in metadata['functions'] if f['name'] == spec['function'])
            if args.mode == 'invoke':
                invoke(args.job, spec, function)
            else:
                execute(args.job, spec, function)
    except Exception as error:
        current = json.loads((args.job / 'progress.json').read_text())['stage']
        code = dict(inspection='invalid_input', compilation='compilation_failed', execution='execution_failed', simulation='simulation_failed')[current]
        if 'trace_limit_exceeded' in str(error):
            code = 'trace_limit_exceeded'
        write_json(args.job / 'error.json', dict(code=code, message=str(error), stage=current))
        print(str(error), file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
