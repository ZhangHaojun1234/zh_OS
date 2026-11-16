#!/usr/bin/env bash
# ============================================================================
# zh_OS 1.0 (x86_64)
# File:    check.sh
# Version: 1.0
# Brief:   只读环境体检：打印 gcc/nasm/grub-mkrescue/xorriso/qemu 版本与修复建议。
# ============================================================================

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
ENV_MODE_FILE="$ROOT/env.mode"
TOOLCHAIN_DIR="$ROOT/toolchain"
TARGET="x86_64-elf"
VERSION_TEXT="zh_OS 1.0 (x86_64)"   # 与 kernel/version.h 保持一致

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  C_RESET=$'\033[0m'
  C_YELLOW=$'\033[33m'
  C_CYAN=$'\033[36m'
else
  C_RESET=""
  C_YELLOW=""
  C_CYAN=""
fi

info() { printf '%s[信息]%s %s\n' "$C_CYAN" "$C_RESET" "$*"; }
warn() { printf '%s[警告]%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }

if [[ "${EUID}" -eq 0 ]]; then
  SUDO_HINT=""
else
  SUDO_HINT="sudo "
fi

read_env_mode() {
  [[ -f "$ENV_MODE_FILE" ]] || return 0
  tr -d '[:space:]' <"$ENV_MODE_FILE" || true
}

first_line() {
  local text="$1"
  text="${text%%$'\n'*}"
  printf '%s' "$text"
}

version_of() {
  local out=""
  out="$("$@" 2>&1 || true)"
  out="$(first_line "$out")"
  if [[ -z "$out" ]]; then
    printf '版本未知'
  else
    printf '%s' "$out"
  fi
}

tool_version() {
  local tool="$1"
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf '未安装'
    return 0
  fi
  case "$tool" in
    nasm) version_of nasm -v ;;
    grub-mkrescue) version_of grub-mkrescue --version ;;
    xorriso) version_of xorriso -version ;;
    qemu-system-x86_64) version_of qemu-system-x86_64 --version ;;
    *) version_of "$tool" --version ;;
  esac
}

distro_label() {
  local name="${WSL_DISTRO_NAME:-}"
  local pretty=""
  if [[ -r /etc/os-release ]]; then
    pretty="$(
      # shellcheck disable=SC1091
      . /etc/os-release
      printf '%s' "${PRETTY_NAME:-}"
    )" || pretty=""
  fi
  if [[ -n "$name" && -n "$pretty" ]]; then
    if [[ "$pretty" == "$name"* ]]; then
      printf '%s' "$pretty"
    else
      printf '%s（%s）' "$name" "$pretty"
    fi
  elif [[ -n "$name" ]]; then
    printf '%s' "$name"
  elif [[ -n "$pretty" ]]; then
    printf '%s' "$pretty"
  else
    printf '未知 Linux'
  fi
}

gcc_version_for() {
  local mode="$1"
  local cc=""
  if [[ "$mode" == "cross" ]]; then
    cc="$TOOLCHAIN_DIR/bin/$TARGET-gcc"
  else
    cc="gcc"
  fi
  if command -v "$cc" >/dev/null 2>&1; then
    version_of "$cc" --version
  else
    printf '未安装'
  fi
}

print_report() {
  local mode="$1"

  printf '\n'
  printf '========================================\n'
  printf ' %s 环境检查报告\n' "$VERSION_TEXT"
  printf '========================================\n'
  printf 'WSL 发行版        : %s\n' "$(distro_label)"
  printf '项目根目录        : %s\n' "$ROOT"
  printf 'env.mode          : %s\n' "${mode:-未配置}"
  case "$mode" in
    cross)
      printf '工具链路线        : 交叉工具链（%s，安装于 toolchain/）\n' "$TARGET"
      ;;
    system-gcc)
      printf '工具链路线        : 系统 gcc（-m64 -ffreestanding -nostdlib）\n'
      ;;
    *)
      printf '工具链路线        : 未配置\n'
      ;;
  esac
  printf 'gcc               : %s\n' "$(gcc_version_for "${mode:-none}")"
  printf 'nasm              : %s\n' "$(tool_version nasm)"
  printf 'grub-mkrescue     : %s\n' "$(tool_version grub-mkrescue)"
  printf 'xorriso           : %s\n' "$(tool_version xorriso)"
  printf 'qemu-system-x86_64: %s\n' "$(tool_version qemu-system-x86_64)"
  printf '========================================\n'
}

main() {
  local mode=""
  local rc=0
  local tool
  local missing=()
  local gcc_ok=1

  mode="$(read_env_mode)"

  printf '%s\n' "$VERSION_TEXT 环境检查（只检测，不安装任何软件）"

  if command -v gcc >/dev/null 2>&1; then
    info "找到 gcc：$(gcc_version_for "${mode:-none}")"
  fi

  for tool in grub-mkrescue xorriso nasm qemu-system-x86_64; do
    if command -v "$tool" >/dev/null 2>&1; then
      info "找到 $tool：$(tool_version "$tool")"
    else
      warn "缺少 $tool"
      missing+=("$tool")
    fi
  done

  case "$mode" in
    "")
      warn "未找到 env.mode，尚未选择工具链路线。"
      rc=1
      ;;
    system-gcc)
      if ! command -v gcc >/dev/null 2>&1; then
        warn "env.mode 为 system-gcc，但未找到 gcc。"
        gcc_ok=0
      fi
      ;;
    cross)
      if [[ ! -x "$TOOLCHAIN_DIR/bin/$TARGET-gcc" ]]; then
        warn "env.mode 为 cross，但未找到 $TOOLCHAIN_DIR/bin/$TARGET-gcc。"
        gcc_ok=0
      fi
      ;;
    *)
      warn "env.mode 内容无法识别：$mode"
      rc=1
      ;;
  esac

  if ((gcc_ok == 0)); then
    rc=1
  fi
  if ((${#missing[@]} > 0)); then
    rc=1
  fi

  print_report "$mode"

  if ((${#missing[@]} > 0)); then
    printf '\n修复命令（在 WSL 内执行）：\n'
    printf '  %sapt-get update\n' "$SUDO_HINT"
    printf '  %sapt-get install -y build-essential nasm grub-pc-bin grub-common xorriso mtools qemu-system-x86\n' "$SUDO_HINT"
  fi

  if [[ -z "$mode" ]]; then
    printf '\n环境尚未初始化，请先运行：./setup.sh\n'
  elif ((gcc_ok == 0)); then
    if [[ "$mode" == "cross" ]]; then
      printf '\n工具链不完整，请重新运行：./setup.sh --build-cross-toolchain\n'
    else
      printf '\n工具链不完整，请重新运行：./setup.sh\n'
    fi
  fi

  if ((rc != 0)); then
    printf '\n结论：环境未就绪。\n'
    exit 1
  fi

  printf '\n结论：环境就绪。下一步在 WSL 内运行：./build.sh\n'
  exit 0
}

main "$@"
