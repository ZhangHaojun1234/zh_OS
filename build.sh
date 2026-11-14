#!/usr/bin/env bash
# ============================================================================
# zh_OS 0.1beta (x86_64)
# File:    build.sh
# Version: 0.1beta
# Brief:   一键编译内核并用 grub-mkrescue 生成可启动 ISO。
# ============================================================================

set -euo pipefail
set -E

ROOT="$(cd "$(dirname "$0")" && pwd)"
ENV_MODE_FILE="$ROOT/env.mode"
TOOLCHAIN_DIR="$ROOT/toolchain"
TARGET="x86_64-elf"
VERSION="0.1beta"
ARCH_LABEL="x86_64"
VERSION_TEXT="zh_OS $VERSION ($ARCH_LABEL)"   # 必须与 kernel/version.h 的 ZH_OS_VERSION_TEXT 一致
ISO_NAME="zh_OS-$VERSION.iso"
BUILD_DIR="$ROOT/build"
ISO_DIR="$ROOT/iso"
ELF="$BUILD_DIR/zh_OS.elf"
KERNEL_NAME="zh_OS.elf"

CFLAGS=(
  -m64
  -ffreestanding
  -fno-pie
  -mno-red-zone
  -mcmodel=small
  -Wall
  -Wextra
  -fno-stack-protector
  -nostdlib
)

# 可选：ZH_VGA_SELFTEST=1 时编入 16 色调色板自检（默认关闭，保持 Linux 风格的单色输出）
if [[ "${ZH_VGA_SELFTEST:-0}" == "1" ]]; then
  CFLAGS+=(-DZH_VGA_SELFTEST=1)
fi

SOURCES=()
OBJECTS=()

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
DIE_PRINTED=0

on_error() {
  local code="$1" line="$2" cmd="$3"
  if ((DIE_PRINTED == 1)); then
    return 0
  fi
  printf '\n%s[错误]%s 第 %s 行执行失败（退出码 %s）：%s\n' \
    "$C_RED" "$C_RESET" "$line" "$code" "$cmd" >&2
  printf '请根据上方错误信息修复后，重新运行 ./build.sh。\n' >&2
}
trap 'on_error "$?" "$LINENO" "$BASH_COMMAND"' ERR

usage() {
  cat <<'EOF'
用法: ./build.sh [选项]

选项:
  -h, --help   显示本帮助

说明:
  读取项目根目录的 env.mode 决定工具链路线：
    system-gcc  使用系统 gcc / ld
    cross       使用 toolchain/bin/x86_64-elf-gcc 与 x86_64-elf-ld
  产物：zh_OS-0.1beta.iso
EOF
}

run_cmd() {
  # 注意：不要用 `if ! "$@"; then rc=$?`，那时 $? 是 `!` 的结果（恒为 0），
  # 会让编译失败被误报成 exit 0。用 || 分支才能拿到命令的真实退出码。
  "$@" || {
    local rc=$?
    printf '%s[错误]%s 命令失败（退出码 %s）：%s\n' "$C_RED" "$C_RESET" "$rc" "$*" >&2
    exit "$rc"
  }
}

read_env_mode() {
  tr -d '[:space:]' <"$ENV_MODE_FILE" || true
}

setup_toolchain() {
  local mode="$1"
  case "$mode" in
    system-gcc)
      CC="gcc"
      LD="ld"
      ;;
    cross)
      CC="$TOOLCHAIN_DIR/bin/$TARGET-gcc"
      LD="$TOOLCHAIN_DIR/bin/$TARGET-ld"
      [[ -x "$CC" ]] || die "未找到交叉编译器：$CC。请运行 ./setup.sh --build-cross-toolchain，或删除 env.mode 改用 system-gcc。"
      [[ -x "$LD" ]] || die "未找到交叉链接器：$LD。请运行 ./setup.sh --build-cross-toolchain，或删除 env.mode 改用 system-gcc。"
      ;;
    *)
      die "env.mode 内容无法识别：'$mode'。修复：删除 $ENV_MODE_FILE 后重新运行 ./setup.sh"
      ;;
  esac
  command -v nasm >/dev/null 2>&1 || die "未找到 nasm。修复：sudo apt-get install -y nasm"
  command -v grub-mkrescue >/dev/null 2>&1 || die "未找到 grub-mkrescue。修复：sudo apt-get install -y grub-pc-bin grub-common xorriso mtools"
}

# 自动扫描固定目录顶层的 .c/.asm：新增模块放在这些目录即可，无需手工维护编译列表。
# 头文件搜索路径同步覆盖 kernel/ drivers/ users/ shell/，第 5 步的 shell.h 因此可见。
# [ADD] mm/heap.c 位于 mm/ 顶层，由下方 collect_sources() 自动扫描纳入，无需手工登记。
# [ADD] drivers/pit.c 与 shell/cmd_time.c 同样由自动扫描纳入，无需手工登记。
# [ADD] kernel/klog.c 同样由自动扫描纳入，无需手工登记。
# [ADD] kernel/history.c 由自动扫描纳入，无需手工登记。
# [ADD] kernel/console_hist.c 由自动扫描纳入，无需手工登记。
# [ADD] arch/ 是新目录，不在 collect_sources 的自动扫描列表里，这里显式登记
#      arch/cpuinfo.c 的编译路径，不修改下方 collect_sources 的目录列表。
# [ADD] fs/ 同样是新目录，不在自动扫描列表里，下面 main() 处显式登记 fs/ramfs.c。
collect_sources() {
  local dir
  for dir in kernel boot drivers mm shell users; do
    [[ -d "$ROOT/$dir" ]] || continue
    while IFS= read -r -d '' f; do
      SOURCES+=("$f")
    done < <(find "$ROOT/$dir" -maxdepth 1 -type f \( -name '*.c' -o -name '*.asm' \) -print0 | sort -z)
  done
  ((${#SOURCES[@]} > 0)) || die "未找到任何源文件（.c/.asm）。"
}

object_path_for() {
  local src="$1"
  local rel="${src#"$ROOT"/}"
  printf '%s/%s.o' "$BUILD_DIR" "${rel%.*}"
}

compile_sources() {
  local src obj
  for src in "${SOURCES[@]}"; do
    obj="$(object_path_for "$src")"
    mkdir -p "$(dirname "$obj")"
    case "$src" in
      *.c)
        info "编译 ${src#"$ROOT"/}"
        run_cmd "$CC" "${CFLAGS[@]}" -I"$ROOT/kernel" -I"$ROOT/drivers" -I"$ROOT/users" -I"$ROOT/shell" -c "$src" -o "$obj"
        ;;
      *.asm)
        info "汇编 ${src#"$ROOT"/}"
        run_cmd nasm -f elf64 "$src" -o "$obj"
        ;;
      *)
        die "不支持的文件类型：$src"
        ;;
    esac
    OBJECTS+=("$obj")
  done
}

link_kernel() {
  info "链接 $KERNEL_NAME ..."
  run_cmd "$LD" -m elf_x86_64 -T "$ROOT/linker.ld" -o "$ELF" "${OBJECTS[@]}"
  ok "内核已生成：$ELF"
}

# 构建时核对 C 侧与脚本侧的版本字符串，防止只改了一处造成横幅 / uname / 菜单不一致。
check_version_consistency() {
  local header="$ROOT/kernel/version.h"
  [[ -f "$header" ]] || die "缺少统一版本头文件：$header"
  if ! grep -Fq "\"$VERSION_TEXT\"" "$header"; then
    die "版本号不一致：build.sh 的 \"$VERSION_TEXT\" 未出现在 $header 中"
  fi
}

write_grub_cfg() {
  local cfg="$ISO_DIR/boot/grub/grub.cfg"
  mkdir -p "$(dirname "$cfg")"
  cat >"$cfg" <<EOF
# ============================================================================
# zh_OS $VERSION ($ARCH_LABEL)
# File:    iso/boot/grub/grub.cfg
# Version: $VERSION
# Brief:   GRUB2 菜单，用 multiboot2 加载内核 ELF（由 build.sh 自动生成）。
# ============================================================================
set timeout=0
set default=0

menuentry "$VERSION_TEXT" {
    multiboot2 /boot/$KERNEL_NAME
    boot
}
EOF
}

build_iso() {
  info "组织 ISO 目录 ..."
  rm -rf "$ISO_DIR"
  mkdir -p "$ISO_DIR/boot/grub"
  cp "$ELF" "$ISO_DIR/boot/$KERNEL_NAME"
  write_grub_cfg

  info "生成 $ISO_NAME ..."
  run_cmd grub-mkrescue -o "$ROOT/$ISO_NAME" "$ISO_DIR"
}

main() {
  while (($# > 0)); do
    case "$1" in
      -h | --help)
        usage
        exit 0
        ;;
      *) die "未知参数：$1（使用 --help 查看用法）" ;;
    esac
    shift
  done

  [[ -f "$ENV_MODE_FILE" ]] || die "未找到 env.mode。请先运行 ./setup.sh 配置环境。"
  local mode
  mode="$(read_env_mode)"
  [[ -n "$mode" ]] || die "env.mode 为空。请先运行 ./setup.sh 配置环境。"

  setup_toolchain "$mode"
  info "工具链路线：$mode（CC=$CC, LD=$LD）"

  check_version_consistency
  collect_sources
  SOURCES+=("$ROOT/arch/cpuinfo.c")  # [ADD] 显式登记新目录 arch/ 下的源码，不改 collect_sources
  SOURCES+=("$ROOT/fs/ramfs.c")  # [ADD] 显式登记新目录 fs/ 下的源码，不改 collect_sources
  SOURCES+=("$ROOT/fs/fat16.c")  # [ADD] B 组第 3 个功能：fs/ 不在自动扫描列表，显式登记 FAT16
  # [ADD] drivers/ata.c 与 users/users_persist.c 不需要显式登记：collect_sources 的目录列表
  #       已含 drivers/ 与 users/，会自动扫描到这两个文件。若再 SOURCES+= 同路径，链接时会
  #       出现 "multiple definition"（同一 .o 被当两次输入）。故此处仅留说明，不追加重复路径。
  #       （与 arch/、fs/ 不同：那两个是新目录，不在自动扫描列表里，才必须显式登记。）
  compile_sources
  link_kernel
  build_iso

  printf '\n'
  printf '%s\n' '========================================'
  printf ' 构建成功\n'
  printf '%s\n' '========================================'
  printf ' ISO 路径 : %s\n' "$ROOT/$ISO_NAME"
  printf ' 内核 ELF : %s\n' "$ELF"
  printf '%s\n' '----------------------------------------'
  printf ' 测试命令 : qemu-system-x86_64 -cdrom %s -m 128\n' "$ISO_NAME"
  printf '%s\n' '========================================'
}

main "$@"
