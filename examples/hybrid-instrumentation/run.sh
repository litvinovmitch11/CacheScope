#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/../.." && pwd)"
llvm_build_dir="${LLVM_BUILD_DIR:-${repo_dir}/llvm-project/build}"
tool_dir="${MLIR_BIN_DIR:-${llvm_build_dir}/bin}"
build_dir="${BUILD_DIR:-${repo_dir}/build}"
output_root="${CACHESCOPE_OUTPUT_DIR:-${repo_dir}/output}"
output_dir="${output_root}/hybrid-instrumentation"
plugin="${build_dir}/lib/CacheScopePassPlugin.so"

cmake -S "${repo_dir}" -B "${build_dir}" -G Ninja \
  -DMLIR_DIR="${llvm_build_dir}/lib/cmake/mlir" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "${build_dir}"
mkdir -p "${output_dir}"

"${tool_dir}/mlir-opt" "${script_dir}/semantic.mlir" \
  --load-pass-plugin="${plugin}" \
  --pass-pipeline='builtin.module(cachescope-instrument-memory{target-func=kernel})' \
  --mlir-print-debuginfo \
  -o "${output_dir}/semantic.instrumented.mlir"

"${tool_dir}/mlir-opt" "${output_dir}/semantic.instrumented.mlir" \
  --load-pass-plugin="${plugin}" \
  --pass-pipeline='builtin.module(expand-strided-metadata,convert-to-llvm,reconcile-unrealized-casts,cachescope-instrument-llvm-fallback{target-func=kernel})' \
  -o "${output_dir}/semantic.llvm.mlir"

"${tool_dir}/mlir-opt" "${script_dir}/raw-llvm.mlir" \
  --load-pass-plugin="${plugin}" \
  --pass-pipeline='builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})' \
  -o "${output_dir}/fallback.llvm.mlir"
"${tool_dir}/FileCheck" "${script_dir}/raw-llvm.mlir" \
  <"${output_dir}/fallback.llvm.mlir"

if "${tool_dir}/mlir-opt" "${script_dir}/unsupported.mlir" \
  --load-pass-plugin="${plugin}" \
  --pass-pipeline='builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})' \
  >"${output_dir}/unsupported.stdout" \
  2>"${output_dir}/unsupported.stderr"; then
  echo "unsupported example unexpectedly compiled" >&2
  exit 1
fi
"${tool_dir}/FileCheck" "${script_dir}/unsupported.mlir" \
  <"${output_dir}/unsupported.stderr"

echo "Hybrid instrumentation examples OK"
echo "Artifacts: ${output_dir}"
