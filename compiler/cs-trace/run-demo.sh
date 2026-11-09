#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/../.." && pwd)"
llvm_build_dir="${LLVM_BUILD_DIR:-${repo_dir}/llvm-project/build}"
tool_dir="${MLIR_BIN_DIR:-${llvm_build_dir}/bin}"
build_dir="${BUILD_DIR:-${repo_dir}/build}"
output_root="${CACHESCOPE_OUTPUT_DIR:-${repo_dir}/output}"
output_dir="${output_root}/automatic-instrumentation"
live_output_dir="${output_root}/live-demo"

mlir_opt="${tool_dir}/mlir-opt"
mlir_runner="${tool_dir}/mlir-runner"
file_check="${tool_dir}/FileCheck"
mlir_cmake_dir="${llvm_build_dir}/lib/cmake/mlir"

for tool in "${mlir_opt}" "${mlir_runner}" "${file_check}"; do
  if [[ ! -x "${tool}" ]]; then
    echo "Missing tool: ${tool}" >&2
    exit 1
  fi
done

cmake -S "${repo_dir}" -B "${build_dir}" -G Ninja \
  -DMLIR_DIR="${mlir_cmake_dir}" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "${build_dir}"

plugin="${build_dir}/lib/CacheScopePassPlugin.so"
runtime="${build_dir}/compiler/runtime/libcachescope_trace_runtime.so"
pipeline="builtin.module(cachescope-instrument-memory{target-func=matmul_kernel},canonicalize,cse)"

mkdir -p "${output_dir}"

"${mlir_opt}" \
  --load-pass-plugin="${plugin}" \
  "${script_dir}/test/instrument-memory.mlir" \
  --pass-pipeline="builtin.module(cachescope-instrument-memory{target-func=kernel})" \
  | "${file_check}" "${script_dir}/test/instrument-memory.mlir"

"${mlir_opt}" \
  "${script_dir}/test/instrument-llvm-fallback.mlir" \
  --load-pass-plugin="${plugin}" \
  --pass-pipeline="builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})" \
  | "${file_check}" "${script_dir}/test/instrument-llvm-fallback.mlir"

if "${mlir_opt}" \
  --load-pass-plugin="${plugin}" \
  "${script_dir}/test/invalid-rank.mlir" \
  --pass-pipeline="builtin.module(cachescope-instrument-memory{target-func=kernel})" \
  >"${output_dir}/invalid-rank.stdout" 2>"${output_dir}/invalid-rank.stderr"; then
  echo "Expected unsupported-rank instrumentation to fail" >&2
  exit 1
fi
"${file_check}" "${script_dir}/test/invalid-rank.mlir" \
  <"${output_dir}/invalid-rank.stderr"

if "${mlir_opt}" \
  --load-pass-plugin="${plugin}" \
  "${script_dir}/test/unsupported-llvm-memory.mlir" \
  --pass-pipeline="builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})" \
  >"${output_dir}/unsupported-llvm-memory.stdout" \
  2>"${output_dir}/unsupported-llvm-memory.stderr"; then
  echo "Expected unsupported LLVM memory instrumentation to fail" >&2
  exit 1
fi
"${file_check}" "${script_dir}/test/unsupported-llvm-memory.mlir" \
  <"${output_dir}/unsupported-llvm-memory.stderr"

if "${mlir_opt}" \
  --load-pass-plugin="${plugin}" \
  "${script_dir}/test/unknown-llvm-call.mlir" \
  --pass-pipeline="builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})" \
  >"${output_dir}/unknown-llvm-call.stdout" \
  2>"${output_dir}/unknown-llvm-call.stderr"; then
  echo "Expected unknown LLVM call instrumentation to fail" >&2
  exit 1
fi
"${file_check}" "${script_dir}/test/unknown-llvm-call.mlir" \
  <"${output_dir}/unknown-llvm-call.stderr"

if [[ ! -f "${live_output_dir}/matmul-baseline.loops.mlir" ||
      ! -f "${live_output_dir}/matmul-tile4-ikj.loops.mlir" ]]; then
  MLIR_BIN_DIR="${tool_dir}" CACHESCOPE_OUTPUT_DIR="${output_root}" \
    "${repo_dir}/examples/live-demo/run-demo.sh"
fi
for variant in matmul-baseline matmul-tile4-ikj; do
  loops="${live_output_dir}/${variant}.loops.mlir"
  instrumented="${output_dir}/${variant}.instrumented.mlir"
  llvm_ir="${output_dir}/${variant}.llvm.mlir"
  trace="${output_dir}/${variant}.trace.json"

  "${mlir_opt}" "${loops}" \
    --load-pass-plugin="${plugin}" \
    --pass-pipeline="${pipeline}" \
    --mlir-print-debuginfo \
    -o "${instrumented}"

  "${mlir_opt}" "${instrumented}" \
    --load-pass-plugin="${plugin}" \
    --pass-pipeline="builtin.module(expand-strided-metadata,lower-affine,convert-scf-to-cf,convert-to-llvm,reconcile-unrealized-casts,cachescope-instrument-llvm-fallback{target-func=matmul_kernel})" \
    -o "${llvm_ir}"

  actual="$(
    CACHESCOPE_TRACE_FILE="${trace}" \
    CACHESCOPE_VARIANT="${variant}" \
    "${mlir_runner}" "${llvm_ir}" \
      -e main \
      -entry-point-result=i32 \
      --shared-libs="${runtime}" \
      | tail -n 1 \
      | tr -d '[:space:]'
  )"
  if [[ "${actual}" != "16" ]]; then
    echo "${variant}: expected result 16, got ${actual}" >&2
    exit 1
  fi

  python3 "${repo_dir}/examples/manual-instrumentation/verify_trace.py" "${trace}"
  echo "${variant}: result OK (${actual})"
done

python3 "${script_dir}/test/compare_traces.py" \
  "${output_dir}/matmul-baseline.trace.json" \
  "${output_dir}/matmul-tile4-ikj.trace.json"

echo "automatic instrumentation demo OK"
echo "artifacts: ${output_dir}"
