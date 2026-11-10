#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/.." && pwd)"
llvm_major="$(
  sed -n 's/^set(CACHESCOPE_LLVM_MAJOR_VERSION "\([0-9]*\)")$/\1/p' \
    "${repo_dir}/cmake/LLVMVersion.cmake"
)"
if [[ -z "${llvm_major}" ]]; then
  echo "Cannot read the pinned LLVM major version" >&2
  exit 1
fi
llvm_root="${LLVM_ROOT:-/usr/lib/llvm-${llvm_major}}"
mlir_dir="${MLIR_DIR:-${llvm_root}/lib/cmake/mlir}"
build_dir="${BUILD_DIR:-${repo_dir}/build}"
with_torch=0
with_lint=0
build_cache_model="${CACHESCOPE_BUILD_CACHE_MODEL:-ON}"

case "${build_cache_model}" in
  ON|OFF)
    ;;
  *)
    echo "CACHESCOPE_BUILD_CACHE_MODEL must be ON or OFF" >&2
    exit 2
    ;;
esac

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

export CC="${CC:-${llvm_root}/bin/clang}"
export CXX="${CXX:-${llvm_root}/bin/clang++}"

cmake -S "${repo_dir}" -B "${build_dir}" -G Ninja \
  -DMLIR_DIR="${mlir_dir}" \
  -DCACHESCOPE_CLANG_TOOLS_DIR="${llvm_root}/bin" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCACHESCOPE_BUILD_CACHE_MODEL="${build_cache_model}" \
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
  LLVM_ROOT="${llvm_root}" \
    BUILD_DIR="${build_dir}" \
    "${python_bin}" "${repo_dir}/examples/torch-demo/run.py"
fi

echo "CacheScope checks passed"
