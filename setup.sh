#!/usr/bin/env bash
# ============================================================================
# zh_OS 0.1beta (x86_64)
# File:    setup.sh
# Version: 0.1beta
# Brief:   安装 WSL 构建依赖、可选编译交叉工具链并写入 env.mode。
# ============================================================================

set -euo pipefail
set -E

ROOT="$(cd "$(dirname "$0")" && pwd)"
ENV_MODE_FILE="$ROOT/env.mode"
TOOLCHAIN_DIR="$ROOT/toolchain"
TARGET="x86_64-elf"
BINUTILS_VERSION="${BINUTILS_VERSION:-2.42}"
GCC_VERSION="${GCC_VERSION:-13.2.0}"
SRC_CACHE="$ROOT/.cache/src"
BUILD_CACHE="$ROOT/.cache/build"
LOG_DIR="$ROOT/.cache/logs"

APT_PACKAGES=(build-essential nasm grub-pc-bin grub-common xorriso mtools qemu-system-x86)
CROSS_APT_PACKAGES=(flex bison texinfo libgmp-dev libmpfr-dev libmpc-dev xz-utils curl ca-certificates)

BUILD_CROSS=0
APT_UPDATED=0
DIE_PRINTED=0
SUDO=()
SUDO_HINT=""
CLEANUP_DIRS=()
JOBS="$(nproc 2>/dev/null || echo 2)"

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  C_RESET=$'\033[0m'
  C_RED=$'\033[31m'
  C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'
  C_CYAN=$'\033[36m'
else
  C_RESET=""
  C_RED=""
  C_GREEN=""
  C_YELLOW=""
  C_CYAN=""
fi

info() { printf '%s[信息]%s %s\n' "$C_CYAN" "$C_RESET" "$*"; }
ok() { printf '%s[完成]%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn() { printf '%s[警告]%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
die() {
  DIE_PRINTED=1
  printf '%s[错误]%s %s\n' "$C_RED" "$C_RESET" "$*" >&2
  exit 1
}

cleanup() {
  local d
  for d in ${CLEANUP_DIRS[@]+"${CLEANUP_DIRS[@]}"}; do
    if [[ -n "$d" ]]; then
      rm -rf "$d"
    fi
  done
}
trap cleanup EXIT

on_error() {
  local code="$1" line="$2" cmd="$3"
  if ((DIE_PRINTED == 1)); then
    return 0
  fi
  printf '\n%s[错误]%s 第 %s 行执行失败（退出码 %s）：%s\n' \
    "$C_RED" "$C_RESET" "$line" "$code" "$cmd" >&2
  printf '请根据上方错误信息修复后，重新运行 ./setup.sh。\n' >&2
}
trap 'on_error "$?" "$LINENO" "$BASH_COMMAND"' ERR

usage() {
  cat <<'EOF'
用法: ./setup.sh [选项]

选项:
  --build-cross-toolchain  从源码编译 x86_64-elf 交叉工具链到项目 toolchain/
                           （耗时约 20-60 分钟，会先请求确认）
  -h, --help               显示本帮助

环境变量:
  BINUTILS_VERSION  指定 binutils 版本（默认 2.42）
  GCC_VERSION       指定 gcc 版本（默认 13.2.0）
  NO_COLOR          设置后关闭彩色输出

说明:
  本脚本必须在 WSL2 Ubuntu 中运行，仅支持 Ubuntu/Debian 的 apt 包管理器。
  默认使用系统 gcc（system-gcc），重复运行不会重复安装软件包或重写 env.mode。
EOF
}

parse_args() {
  while (($# > 0)); do
    case "$1" in
      --build-cross-toolchain) BUILD_CROSS=1 ;;
      -h | --help)
        usage
        exit 0
        ;;
      *) die "未知参数：$1（使用 --help 查看用法）" ;;
    esac
    shift
  done
}

require_wsl() {
  if [[ -n "${WSL_DISTRO_NAME:-}" ]]; then
    return 0
  fi
  if [[ -r /proc/version ]] && grep -qi 'microsoft' /proc/version; then
    return 0
  fi
  printf '请在 WSL2 Ubuntu 中运行本脚本\n' >&2
  exit 1
}

require_apt() {
  command -v apt-get >/dev/null 2>&1 ||
    die "未找到 apt-get。本脚本仅支持 Ubuntu/Debian，请使用 WSL2 Ubuntu 发行版。"
  command -v dpkg-query >/dev/null 2>&1 ||
    die "未找到 dpkg-query。请确认当前系统是 Ubuntu/Debian。"
}

setup_sudo() {
  if [[ "${EUID}" -eq 0 ]]; then
    SUDO=()
    SUDO_HINT=""
    return 0
  fi
  SUDO_HINT="sudo "
  if command -v sudo >/dev/null 2>&1; then
    SUDO=(sudo)
  else
    SUDO=()
    warn "未找到 sudo：仅在需要安装软件包或写入系统目录时才会失败。"
  fi
}

pkg_installed() {
  dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'install ok installed'
}

run_priv() {
  if [[ "${EUID}" -eq 0 ]]; then
    "$@"
    return 0
  fi
  if ((${#SUDO[@]} > 0)); then
    "${SUDO[@]}" "$@"
    return 0
  fi
  die "需要管理员权限安装软件包，但未找到 sudo。修复：安装 sudo 后重试，或以 root 运行：su -c 'bash setup.sh'"
}

install_apt_packages() {
  local pkgs=("$@")
  local missing=()
  local p

  for p in "${pkgs[@]}"; do
    pkg_installed "$p" || missing+=("$p")
  done

  if ((${#missing[@]} == 0)); then
    ok "软件包已安装，跳过：${pkgs[*]}"
    return 0
  fi

  info "缺失软件包：${missing[*]}"
  if [[ -n "$SUDO_HINT" ]]; then
    info "安装软件包需要管理员权限，可能会提示输入 sudo 密码。"
  fi

  if ((APT_UPDATED == 0)); then
    info "更新 apt 软件包索引（${SUDO_HINT}apt-get update）..."
    if ! run_priv env DEBIAN_FRONTEND=noninteractive apt-get update; then
      die "apt-get update 失败。请检查网络或软件源后手动执行：${SUDO_HINT}apt-get update"
    fi
    APT_UPDATED=1
  fi

  info "安装：${missing[*]}"
  if ! run_priv env DEBIAN_FRONTEND=noninteractive apt-get install -y "${missing[@]}"; then
    die "软件包安装失败。请手动执行：${SUDO_HINT}apt-get install -y ${missing[*]}"
  fi
  ok "软件包安装完成：${missing[*]}"
}

read_env_mode() {
  [[ -f "$ENV_MODE_FILE" ]] || return 0
  tr -d '[:space:]' <"$ENV_MODE_FILE" || true
}

write_env_mode() {
  local mode="$1"
  local force="${2:-0}"
  local current=""
  if [[ -f "$ENV_MODE_FILE" ]]; then
    current="$(read_env_mode)"
  fi

  if [[ "$current" == "$mode" ]]; then
    ok "env.mode 已为 $mode，跳过写入。"
    return 0
  fi

  if [[ -n "$current" && "$force" != "1" ]]; then
    warn "env.mode 已存在（$current），按幂等策略不覆盖。"
    warn "如需切换工具链路线，请先删除 $ENV_MODE_FILE 再运行。"
    return 0
  fi

  if [[ -n "$current" ]]; then
    info "按显式请求更新 env.mode：$current -> $mode"
  fi

  printf '%s\n' "$mode" >"$ENV_MODE_FILE"
  ok "已写入 env.mode：$mode"
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

try_compile() {
  local cc="$1" label="$2"
  local tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/zh_os.XXXXXX")"
  CLEANUP_DIRS+=("$tmp")
  printf 'int main(void) { return 0; }\n' >"$tmp/main.c"

  if "$cc" -m64 -ffreestanding -nostdlib -c "$tmp/main.c" -o "$tmp/main.o" \
    >"$tmp/cc.log" 2>&1 && [[ -f "$tmp/main.o" ]]; then
    return 0
  fi

  warn "$label 验证失败，编译器输出："
  sed 's/^/    /' "$tmp/cc.log" >&2 || true
  return 1
}

verify_system_gcc() {
  if ! command -v gcc >/dev/null 2>&1; then
    warn "未找到 gcc。修复：${SUDO_HINT}apt-get install -y build-essential"
    return 1
  fi

  info "验证系统 gcc：gcc -m64 -ffreestanding -nostdlib -c ..."
  if try_compile gcc "系统 gcc"; then
    ok "系统 gcc 可用：$(version_of gcc --version)"
    return 0
  fi

  warn "系统 gcc 无法完成 64 位裸机编译。"
  warn "修复：${SUDO_HINT}apt-get install -y build-essential"
  warn "并确认当前 Ubuntu 为 64 位（uname -m 应输出 x86_64）。"
  return 1
}

cross_toolchain_ok() {
  local cc="$TOOLCHAIN_DIR/bin/$TARGET-gcc"
  local libgcc=""

  [[ -x "$cc" ]] || return 1
  libgcc="$("$cc" -print-libgcc-file-name 2>/dev/null || true)"
  [[ -n "$libgcc" && -f "$libgcc" ]] || return 1
  try_compile "$cc" "交叉 gcc" || return 1
  return 0
}

download() {
  local out="$1"
  shift
  local urls=("$@")
  local url

  if [[ -s "$out" ]] && tar -tf "$out" >/dev/null 2>&1; then
    ok "使用缓存源码：$(basename "$out")"
    return 0
  fi

  mkdir -p "$(dirname "$out")"

  if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
    die "未找到 curl 或 wget。修复：${SUDO_HINT}apt-get install -y curl"
  fi

  for url in "${urls[@]}"; do
    info "下载 $url"
    rm -f "$out.part"
    if command -v curl >/dev/null 2>&1; then
      if curl -fL --retry 3 --connect-timeout 20 -o "$out.part" "$url"; then
        mv -f "$out.part" "$out"
        break
      fi
    else
      if wget -O "$out.part" "$url"; then
        mv -f "$out.part" "$out"
        break
      fi
    fi
    rm -f "$out.part"
  done

  if ! tar -tf "$out" >/dev/null 2>&1; then
    rm -f "$out"
    die "源码下载失败或文件损坏。请检查网络，或手动下载后放到 $SRC_CACHE"
  fi
  ok "下载完成：$(basename "$out")"
}

extract_tarball() {
  local tarball="$1" srcdir="$2"
  if [[ -d "$srcdir" ]]; then
    ok "源码已解压：$srcdir"
    return 0
  fi
  info "解压 $(basename "$tarball") ..."
  tar -xf "$tarball" -C "$SRC_CACHE" || die "解压失败：$tarball"
  [[ -d "$srcdir" ]] || die "解压后未找到目录：$srcdir"
}

run_logged() {
  local log="$1"
  shift
  if ! "$@" >>"$log" 2>&1; then
    warn "命令执行失败：$*"
    warn "日志尾部（$log）："
    sed 's/^/    /' "$log" | tail -n 40 >&2 || true
    return 1
  fi
}

confirm_cross_build() {
  if [[ ! -t 0 ]]; then
    die "无法在非交互终端确认交叉工具链编译。请在 WSL 终端中直接运行 ./setup.sh --build-cross-toolchain"
  fi

  cat <<EOF
即将从源码编译交叉工具链：
  binutils : $BINUTILS_VERSION
  gcc      : $GCC_VERSION
  目标平台 : $TARGET
  安装位置 : $TOOLCHAIN_DIR

预计耗时：20-60 分钟（取决于 CPU 核心数和网络下载速度）。
编译期间 CPU 占用较高，请耐心等待。
EOF

  if [[ "$ROOT" == /mnt/* ]]; then
    warn "项目位于 Windows 盘（$ROOT），在 drvfs 上编译会明显变慢。"
    warn "建议将项目复制到 WSL 家目录（例如 ~/zh_OS）后再编译交叉工具链。"
  fi

  local answer=""
  read -r -p "是否继续？输入 y 继续，其他任意输入取消: " answer || true
  case "$answer" in
    y | Y | yes | YES) ;;
    *) die "已取消交叉工具链编译（未修改 env.mode）。" ;;
  esac
}

build_binutils() {
  local tb="$SRC_CACHE/binutils-$BINUTILS_VERSION.tar.xz"
  local src="$SRC_CACHE/binutils-$BINUTILS_VERSION"
  local bdir="$BUILD_CACHE/binutils-$BINUTILS_VERSION"
  local log="$LOG_DIR/binutils-$BINUTILS_VERSION.log"

  download "$tb" \
    "https://ftpmirror.gnu.org/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
    "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz"
  extract_tarball "$tb" "$src"
  mkdir -p "$bdir"

  if [[ ! -f "$bdir/Makefile" ]]; then
    info "配置 binutils ..."
    run_logged "$log" "$src/configure" \
      --target="$TARGET" \
      --prefix="$TOOLCHAIN_DIR" \
      --with-sysroot \
      --disable-nls \
      --disable-werror ||
      die "binutils 配置失败。日志：$log"
  else
    ok "binutils 已配置，继续编译。"
  fi

  info "编译 binutils（-j$JOBS，日志：$log）..."
  run_logged "$log" make -C "$bdir" -j"$JOBS" ||
    die "binutils 编译失败。请查看日志：$log"
  info "安装 binutils 到 $TOOLCHAIN_DIR ..."
  run_logged "$log" make -C "$bdir" install ||
    die "binutils 安装失败。请查看日志：$log"
  ok "binutils $BINUTILS_VERSION 安装完成。"
}

build_gcc() {
  local tb="$SRC_CACHE/gcc-$GCC_VERSION.tar.xz"
  local src="$SRC_CACHE/gcc-$GCC_VERSION"
  local bdir="$BUILD_CACHE/gcc-$GCC_VERSION"
  local log="$LOG_DIR/gcc-$GCC_VERSION.log"

  download "$tb" \
    "https://ftpmirror.gnu.org/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
    "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz"
  extract_tarball "$tb" "$src"
  mkdir -p "$bdir"
  export PATH="$TOOLCHAIN_DIR/bin:$PATH"

  if [[ ! -f "$bdir/Makefile" ]]; then
    info "配置 gcc（目标 $TARGET，仅 C 语言，裸机）..."
    run_logged "$log" "$src/configure" \
      --target="$TARGET" \
      --prefix="$TOOLCHAIN_DIR" \
      --disable-nls \
      --enable-languages=c \
      --without-headers \
      --disable-werror ||
      die "gcc 配置失败。日志：$log"
  else
    ok "gcc 已配置，继续编译。"
  fi

  info "编译 gcc 主体（-j$JOBS，日志：$log）..."
  run_logged "$log" make -C "$bdir" -j"$JOBS" all-gcc ||
    die "gcc 编译失败。请查看日志：$log"
  info "编译 libgcc ..."
  run_logged "$log" make -C "$bdir" -j"$JOBS" all-target-libgcc ||
    die "libgcc 编译失败。请查看日志：$log（常见原因：缺少 GMP/MPFR/MPC 开发包）"
  info "安装 gcc 与 libgcc ..."
  run_logged "$log" make -C "$bdir" install-gcc ||
    die "gcc 安装失败。请查看日志：$log"
  run_logged "$log" make -C "$bdir" install-target-libgcc ||
    die "libgcc 安装失败。请查看日志：$log"
  ok "gcc $GCC_VERSION 安装完成。"
}

build_cross_toolchain() {
  if cross_toolchain_ok >/dev/null 2>&1; then
    ok "交叉工具链已安装且可用，跳过编译。"
    return 0
  fi

  confirm_cross_build
  install_apt_packages "${CROSS_APT_PACKAGES[@]}"
  mkdir -p "$SRC_CACHE" "$BUILD_CACHE" "$LOG_DIR"

  build_binutils
  build_gcc

  export PATH="$TOOLCHAIN_DIR/bin:$PATH"
  if ! cross_toolchain_ok; then
    die "交叉工具链自检失败：无法用 $TOOLCHAIN_DIR/bin/$TARGET-gcc 编译测试程序。日志目录：$LOG_DIR"
  fi
  ok "交叉工具链验证通过：$TOOLCHAIN_DIR/bin/$TARGET-gcc"
}

setup_toolchain() {
  if ((BUILD_CROSS == 1)); then
    build_cross_toolchain
    write_env_mode cross 1
    return 0
  fi

  local mode=""
  if [[ -f "$ENV_MODE_FILE" ]]; then
    mode="$(read_env_mode)"
  fi

  case "$mode" in
    "")
      info "未发现 env.mode，按默认路线 system-gcc 检测。"
      verify_system_gcc ||
        die "默认路线 system-gcc 不可用。请安装 64 位 gcc：${SUDO_HINT}apt-get install -y build-essential"
      write_env_mode system-gcc
      ;;
    system-gcc)
      verify_system_gcc ||
        die "env.mode 为 system-gcc，但系统 gcc 不可用。修复后重新运行，或删除 env.mode 再检测。"
      ok "env.mode 已是 system-gcc，无需重写。"
      ;;
    cross)
      if ! cross_toolchain_ok 2>/dev/null; then
        die "env.mode 为 cross，但交叉工具链不可用。修复：重新运行 ./setup.sh --build-cross-toolchain，或删除 env.mode 后使用系统 gcc。"
      fi
      ok "交叉工具链可用（env.mode=cross），无需重写。"
      ;;
    *)
      die "env.mode 内容无法识别：$mode。修复：删除 $ENV_MODE_FILE 后重新运行 ./setup.sh"
      ;;
  esac
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

check_required_tools() {
  local tool
  local missing=()

  for tool in grub-mkrescue xorriso nasm qemu-system-x86_64; do
    if command -v "$tool" >/dev/null 2>&1; then
      ok "自检通过：$tool（$(tool_version "$tool")）"
    else
      warn "自检失败：缺少 $tool"
      missing+=("$tool")
    fi
  done

  if ((${#missing[@]} > 0)); then
    printf '\n修复命令：\n' >&2
    printf '  %sapt-get update\n' "$SUDO_HINT" >&2
    printf '  %sapt-get install -y build-essential nasm grub-pc-bin grub-common xorriso mtools qemu-system-x86\n' "$SUDO_HINT" >&2
    return 1
  fi
  return 0
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
  local mode
  mode="$(read_env_mode)"
  [[ -n "$mode" ]] || mode="未配置"

  printf '\n'
  printf '========================================\n'
  printf ' zh_OS 环境就绪报告\n'
  printf '========================================\n'
  printf 'WSL 发行版        : %s\n' "$(distro_label)"
  printf '项目根目录        : %s\n' "$ROOT"
  printf 'env.mode          : %s\n' "$mode"
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
  printf 'gcc               : %s\n' "$(gcc_version_for "$mode")"
  printf 'nasm              : %s\n' "$(tool_version nasm)"
  printf 'grub-mkrescue     : %s\n' "$(tool_version grub-mkrescue)"
  printf 'xorriso           : %s\n' "$(tool_version xorriso)"
  printf 'qemu-system-x86_64: %s\n' "$(tool_version qemu-system-x86_64)"
  printf '%s\n' '----------------------------------------'
  printf '下一步            : 在 WSL 内运行 ./build.sh\n'
  if [[ ! -x "$ROOT/setup.sh" || ! -x "$ROOT/check.sh" ]]; then
    printf '提示              : 脚本没有可执行权限，请先运行 chmod +x setup.sh check.sh（或直接用 bash setup.sh）\n'
  fi
  printf '========================================\n'
}

main() {
  require_wsl
  parse_args "$@"
  require_apt
  setup_sudo

  info "项目根目录：$ROOT"
  info "检测到 WSL 发行版：$(distro_label)"

  install_apt_packages "${APT_PACKAGES[@]}"
  setup_toolchain

  if ! check_required_tools; then
    die "环境自检未通过：请按上方修复命令安装缺失工具后重新运行 ./setup.sh"
  fi

  print_report
  ok "环境就绪。"
}

main "$@"
