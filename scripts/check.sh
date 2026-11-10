#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/.." && pwd)"
llvm_build_dir="${LLVM_BUILD_DIR:-${repo_dir}/llvm-project/build}"
build_dir="${BUILD_DIR:-${repo_dir}/build}"
with_torch=0
with_lint=0

for option in "$@"; do
  case "${option}" in
    --with-torch)
      with_torch=1
      ;;
    --with-lint)
      with_lint=1
      ;;
    *)
      echo "usage: $0 [--with-torch] [--with-lint]" >&2
      exit 2
      ;;
  esac
done

cmake -S "${repo_dir}" -B "${build_dir}" -G Ninja \
  -DMLIR_DIR="${llvm_build_dir}/lib/cmake/mlir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build "${build_dir}"
ctest --test-dir "${build_dir}" --output-on-failure

if [[ "${with_lint}" -eq 1 ]]; then
  cmake --build "${build_dir}" --target format-check tidy
fi

if [[ "${with_torch}" -eq 1 ]]; then
  python_bin="${PYTHON_BIN:-${repo_dir}/venv/bin/python}"
  if [[ ! -x "${python_bin}" ]]; then
    echo "Python environment is missing: ${python_bin}" >&2
    exit 1
  fi
  LLVM_BUILD_DIR="${llvm_build_dir}" \
    BUILD_DIR="${build_dir}" \
    "${python_bin}" "${repo_dir}/examples/torch-demo/run.py"
fi

echo "CacheScope checks passed"
