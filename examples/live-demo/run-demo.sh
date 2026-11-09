#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/../.." && pwd)"
tool_dir="${MLIR_BIN_DIR:-${repo_dir}/llvm-project/build/bin}"
out_dir="${script_dir}/generated"

mlir_opt="${tool_dir}/mlir-opt"
transform_opt="${tool_dir}/mlir-transform-opt"
mlir_runner="${tool_dir}/mlir-runner"

for tool in "${mlir_opt}" "${transform_opt}" "${mlir_runner}"; do
  if [[ ! -x "${tool}" ]]; then
    echo "Missing tool: ${tool}" >&2
    echo "Set MLIR_BIN_DIR to the directory containing MLIR executables." >&2
    exit 1
  fi
done

mkdir -p "${out_dir}"

lower_to_loops() {
  local input="$1"
  local output="$2"
  "${mlir_opt}" "${input}" \
    --convert-linalg-to-loops \
    --canonicalize \
    --cse \
    -o "${output}"
}

apply_transform() {
  local payload="$1"
  local schedule="$2"
  local output="$3"
  "${transform_opt}" "${payload}" \
    --transform="${schedule}" \
    | "${mlir_opt}" - -o "${output}"
}

compile_and_run() {
  local loops="$1"
  local llvm_ir="$2"
  local expected="$3"

  "${mlir_opt}" "${loops}" \
    --expand-strided-metadata \
    --lower-affine \
    --convert-scf-to-cf \
    --convert-to-llvm \
    --reconcile-unrealized-casts \
    -o "${llvm_ir}"

  local actual
  actual="$("${mlir_runner}" "${llvm_ir}" \
    -e main \
    -entry-point-result=i32 \
    | tail -n 1 \
    | tr -d '[:space:]')"

  if [[ "${actual}" != "${expected}" ]]; then
    echo "Execution failed for ${loops}: expected ${expected}, got ${actual}" >&2
    exit 1
  fi
  echo "  result: ${actual} (expected ${expected})"
}

build_variant() {
  local name="$1"
  local payload="$2"
  local schedule="$3"
  local expected="$4"
  local transformed="${out_dir}/${name}.transformed.mlir"
  local loops="${out_dir}/${name}.loops.mlir"
  local llvm_ir="${out_dir}/${name}.llvm.mlir"

  echo "Building ${name}"
  if [[ -n "${schedule}" ]]; then
    apply_transform "${payload}" "${schedule}" "${transformed}"
  else
    "${mlir_opt}" "${payload}" -o "${transformed}"
  fi
  lower_to_loops "${transformed}" "${loops}"
  compile_and_run "${loops}" "${llvm_ir}" "${expected}"
}

build_variant \
  matmul-baseline \
  "${script_dir}/matmul.mlir" \
  "" \
  16

build_variant \
  matmul-tile4 \
  "${script_dir}/matmul.mlir" \
  "${script_dir}/matmul-tile4.transform.mlir" \
  16

build_variant \
  matmul-tile4-ikj \
  "${script_dir}/matmul.mlir" \
  "${script_dir}/matmul-tile4-ikj.transform.mlir" \
  16

build_variant \
  copy-baseline \
  "${script_dir}/copy.mlir" \
  "" \
  7

build_variant \
  copy-tile4 \
  "${script_dir}/copy.mlir" \
  "${script_dir}/copy-tile4.transform.mlir" \
  7

build_variant \
  copy-tile4-column-first \
  "${script_dir}/copy.mlir" \
  "${script_dir}/copy-tile4-column-first.transform.mlir" \
  7

echo
echo "All live-demo variants compiled and executed successfully."
echo "Readable loop IR is in: ${out_dir}/*.loops.mlir"
echo "LLVM dialect IR is in:  ${out_dir}/*.llvm.mlir"
