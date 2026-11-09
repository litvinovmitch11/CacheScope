#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/../.." && pwd)"
tool_dir="${MLIR_BIN_DIR:-${repo_dir}/llvm-project/build/bin}"
output_root="${CACHESCOPE_OUTPUT_DIR:-${repo_dir}/output}"
build_dir="${output_root}/manual-instrumentation"

mlir_opt="${tool_dir}/mlir-opt"
mlir_runner="${tool_dir}/mlir-runner"

for tool in "${mlir_opt}" "${mlir_runner}"; do
  if [[ ! -x "${tool}" ]]; then
    echo "Missing tool: ${tool}" >&2
    echo "Set MLIR_BIN_DIR to the directory containing MLIR executables." >&2
    exit 1
  fi
done

cmake -S "${script_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "${build_dir}"

llvm_ir="${build_dir}/matmul-manual.llvm.mlir"
trace_file="${build_dir}/manual.trace.json"
runtime_lib="${build_dir}/libcachescope_manual_runtime.so"

"${mlir_opt}" "${script_dir}/matmul-manual.mlir" \
  --expand-strided-metadata \
  --lower-affine \
  --convert-scf-to-cf \
  --convert-to-llvm \
  --reconcile-unrealized-casts \
  -o "${llvm_ir}"

actual="$(
  CACHESCOPE_TRACE_FILE="${trace_file}" \
  CACHESCOPE_VARIANT="manual-baseline" \
  "${mlir_runner}" "${llvm_ir}" \
    -e main \
    -entry-point-result=i32 \
    --shared-libs="${runtime_lib}" \
    | tail -n 1 \
    | tr -d '[:space:]'
)"

if [[ "${actual}" != "16" ]]; then
  echo "Execution failed: expected 16, got ${actual}" >&2
  exit 1
fi

python3 "${script_dir}/verify_trace.py" "${trace_file}"

echo "result OK: ${actual}"
echo "instrumented LLVM dialect: ${llvm_ir}"
echo "trace: ${trace_file}"
