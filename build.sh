#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
ISO_DIR="${BUILD_DIR}/iso"

CC="${CC:-x86_64-elf-gcc}"
LD="${LD:-x86_64-elf-ld}"
AS="${AS:-nasm}"
GRUB_MKRESCUE="${GRUB_MKRESCUE:-grub-mkrescue}"

KERNEL_BIN="${BUILD_DIR}/kernel.bin"
BOOT_OBJ="${BUILD_DIR}/boot.o"
OUT_ISO="${ROOT_DIR}/os.iso"

SRC_DIRS=(kernel memory process gfx ui desktop input shell fs services apps theme debug)

info() {
  printf '[build] %s\n' "$1"
}

fail() {
  printf '[build][error] %s\n' "$1" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "Missing required tool: $1"
}

has_cmd() {
  command -v "$1" >/dev/null 2>&1
}

try_set_tool() {
  local var_name="$1"
  shift
  local candidate

  for candidate in "$@"; do
    if has_cmd "$candidate"; then
      printf -v "$var_name" '%s' "$candidate"
      return 0
    fi
  done

  return 1
}

install_missing_tools_with_apt() {
  local packages=("$@")

  if ! has_cmd apt-get; then
    fail "Missing required tools and apt-get is not available. Install dependencies manually."
  fi

  if [[ "${#packages[@]}" -eq 0 ]]; then
    return 0
  fi

  info "Installing missing dependencies via apt..."
  if has_cmd sudo; then
    sudo apt-get update
    sudo apt-get install -y "${packages[@]}"
  else
    apt-get update
    apt-get install -y "${packages[@]}"
  fi
}

ensure_build_tools() {
  local missing_pkgs=()

  has_cmd "${AS}" || missing_pkgs+=(nasm)
  has_cmd "${GRUB_MKRESCUE}" || missing_pkgs+=(grub-pc-bin xorriso)

  if ! has_cmd "${CC}" && ! has_cmd x86_64-linux-gnu-gcc && ! has_cmd gcc; then
    missing_pkgs+=(gcc gcc-x86-64-linux-gnu)
  fi

  if ! has_cmd "${LD}" && ! has_cmd x86_64-linux-gnu-ld && ! has_cmd ld; then
    missing_pkgs+=(binutils binutils-x86-64-linux-gnu)
  fi

  # Remove duplicate package names.
  if [[ "${#missing_pkgs[@]}" -gt 0 ]]; then
    mapfile -t missing_pkgs < <(printf '%s\n' "${missing_pkgs[@]}" | awk '!seen[$0]++')
    install_missing_tools_with_apt "${missing_pkgs[@]}"
  fi

  if ! has_cmd "${CC}"; then
    try_set_tool CC x86_64-elf-gcc x86_64-linux-gnu-gcc gcc || \
      fail "No usable C compiler found after installation."
  fi

  if ! has_cmd "${LD}"; then
    try_set_tool LD x86_64-elf-ld x86_64-linux-gnu-ld ld || \
      fail "No usable linker found after installation."
  fi

  need_cmd "${AS}"
  need_cmd "${GRUB_MKRESCUE}"
  need_cmd "${CC}"
  need_cmd "${LD}"
}

collect_c_sources() {
  local dir
  for dir in "${SRC_DIRS[@]}"; do
    if [[ -d "${ROOT_DIR}/${dir}" ]]; then
      while IFS= read -r -d '' file; do
        printf '%s\0' "$file"
      done < <(find "${ROOT_DIR}/${dir}" -maxdepth 1 -type f -name '*.c' -print0)
    fi
  done
}

obj_path_for() {
  local src="$1"
  local rel="${src#${ROOT_DIR}/}"
  printf '%s/%s.o' "${BUILD_DIR}" "${rel%.c}"
}

compile_c_file() {
  local src="$1"
  local obj="$2"

  mkdir -p "$(dirname "$obj")"
  "${CC}" \
    -std=gnu11 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pic \
    -m64 \
    -mno-red-zone \
    -Wall \
    -Wextra \
    -I"${ROOT_DIR}/include" \
    -c "$src" \
    -o "$obj"
}

main() {
  info "Checking required tools..."
  ensure_build_tools
  info "Using compiler: ${CC}"
  info "Using linker: ${LD}"

  info "Preparing build directories..."
  rm -rf "${BUILD_DIR}" "${OUT_ISO}"
  mkdir -p "${BUILD_DIR}" "${ISO_DIR}/boot/grub"

  info "Assembling boot module..."
  "${AS}" -f elf64 "${ROOT_DIR}/boot/boot.asm" -o "${BOOT_OBJ}"

  info "Compiling kernel modules..."
  C_OBJS=()
  while IFS= read -r -d '' src; do
    obj="$(obj_path_for "$src")"
    compile_c_file "$src" "$obj"
    C_OBJS+=("$obj")
  done < <(collect_c_sources)

  [[ "${#C_OBJS[@]}" -gt 0 ]] || fail "No C source files were found to build."

  info "Linking kernel binary..."
  "${LD}" -T "${ROOT_DIR}/linker.ld" -nostdlib -o "${KERNEL_BIN}" "${BOOT_OBJ}" "${C_OBJS[@]}"

  info "Preparing ISO tree..."
  cp "${KERNEL_BIN}" "${ISO_DIR}/boot/kernel.bin"
  cp "${ROOT_DIR}/grub/grub.cfg" "${ISO_DIR}/boot/grub/grub.cfg"

  info "Generating bootable ISO..."
  "${GRUB_MKRESCUE}" -o "${OUT_ISO}" "${ISO_DIR}" >/dev/null 2>&1 || \
    fail "grub-mkrescue failed. Ensure xorriso/grub tools are installed."

  info "Build complete: ${OUT_ISO}"
}

main "$@"
