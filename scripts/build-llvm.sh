#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/.." && pwd)"
llvm_source_dir="${repo_dir}/llvm-project/llvm"
llvm_build_dir="${LLVM_BUILD_DIR:-${repo_dir}/llvm-project/build}"
build_type="${LLVM_BUILD_TYPE:-Release}"
version_file="${repo_dir}/cmake/LLVMVersion.cmake"

expected_revision="$(
  sed -n 's/^set(CACHESCOPE_LLVM_REVISION "\([0-9a-f]*\)")$/\1/p' \
    "${version_file}"
)"

if [[ -z "${expected_revision}" ]]; then
  echo "Cannot read the pinned LLVM revision from ${version_file}" >&2
  exit 1
fi

if [[ ! -f "${llvm_source_dir}/CMakeLists.txt" ]]; then
  echo "LLVM submodule is missing. Run: git submodule update --init --recursive" >&2
  exit 1
fi

actual_revision="$(git -C "${repo_dir}/llvm-project" rev-parse HEAD)"
if [[ "${actual_revision}" != "${expected_revision}" ]]; then
  echo "llvm-project is at ${actual_revision}, expected ${expected_revision}." >&2
  echo "Run: git submodule update --init --recursive" >&2
  exit 1
fi

cmake -S "${llvm_source_dir}" -B "${llvm_build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${build_type}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLLVM_ENABLE_PROJECTS='clang;clang-tools-extra;mlir' \
  -DLLVM_TARGETS_TO_BUILD=Native \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DMLIR_ENABLE_BINDINGS_PYTHON=OFF

cmake --build "${llvm_build_dir}" --target \
  FileCheck \
  clang-format \
  clang-tidy \
  llc \
  mlir-opt \
  mlir-runner \
  mlir-transform-opt \
  mlir-translate

echo "LLVM/MLIR tools are ready in ${llvm_build_dir}/bin"
