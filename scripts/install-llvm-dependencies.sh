#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/.." && pwd)"
version_file="${repo_dir}/cmake/LLVMVersion.cmake"

cmake_value() {
  local name="$1"
  sed -n "s/^set(${name} \"\([^\"]*\)\")$/\1/p" "${version_file}"
}

llvm_major="$(cmake_value CACHESCOPE_LLVM_MAJOR_VERSION)"
debian_version="$(cmake_value CACHESCOPE_LLVM_DEBIAN_VERSION)"
if [[ -z "${llvm_major}" || -z "${debian_version}" ]]; then
  echo "Cannot read the pinned LLVM package version from ${version_file}" >&2
  exit 1
fi

source /etc/os-release
if [[ "${ID:-}" != "ubuntu" || "${VERSION_CODENAME:-}" != "noble" ]]; then
  echo "Pinned apt.llvm.org packages require Ubuntu 24.04 (Noble)" >&2
  exit 1
fi

sudo apt-get update
sudo apt-get install -y --no-install-recommends ca-certificates curl gnupg
sudo install -d -m 0755 /etc/apt/keyrings

key_dir="$(mktemp -d)"
trap 'rm -rf "${key_dir}"' EXIT
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
  -o "${key_dir}/apt.llvm.org.asc"

expected_fingerprint="6084F3CF814B57C1CF12EFD515CF4D18AF4F7421"
actual_fingerprint="$(
  gpg --show-keys --with-colons "${key_dir}/apt.llvm.org.asc" \
    | awk -F: '$1 == "fpr" { print $10; exit }'
)"
if [[ "${actual_fingerprint}" != "${expected_fingerprint}" ]]; then
  echo "Unexpected apt.llvm.org signing key: ${actual_fingerprint}" >&2
  exit 1
fi

gpg --dearmor --yes \
  --output "${key_dir}/apt.llvm.org.gpg" \
  "${key_dir}/apt.llvm.org.asc"
sudo install -m 0644 "${key_dir}/apt.llvm.org.gpg" \
  /etc/apt/keyrings/apt.llvm.org.gpg
echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-${llvm_major} main" \
  | sudo tee "/etc/apt/sources.list.d/llvm-${llvm_major}.list" >/dev/null

sudo apt-get update
packages=(
  "clang-${llvm_major}"
  "clang-format-${llvm_major}"
  "clang-tidy-${llvm_major}"
  "libmlir-${llvm_major}-dev"
  "llvm-${llvm_major}-dev"
  "mlir-${llvm_major}-tools"
)
versioned_packages=()
for package in "${packages[@]}"; do
  versioned_packages+=("${package}=${debian_version}")
done
sudo apt-get install -y --no-install-recommends \
  cmake ninja-build python3 "${versioned_packages[@]}"

llvm_root="/usr/lib/llvm-${llvm_major}"
actual_version="$("${llvm_root}/bin/llvm-config" --version)"
expected_version="$(cmake_value CACHESCOPE_LLVM_PACKAGE_VERSION)"
if [[ "${actual_version}" != "${expected_version}" ]]; then
  echo "Installed LLVM ${actual_version}, expected ${expected_version}" >&2
  exit 1
fi

echo "LLVM/MLIR ${actual_version} is ready in ${llvm_root}"
