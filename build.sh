#!/usr/bin/env bash
# ============================================================================
# zh_OS 1.0 (x86_64)
# File:    build.sh
# Version: 1.0
# Brief:   一键编译内核并用 grub-mkrescue 生成可启动 ISO。
# ============================================================================

set -euo pipefail
set -E

ROOT="$(cd "$(dirname "$0")" && pwd)"
ENV_MODE_FILE="$ROOT/env.mode"
TOOLCHAIN_DIR="$ROOT/toolchain"
TARGET="x86_64-elf"
VERSION="1.0"
ARCH_LABEL="x86_64"
VERSION_TEXT="zh_OS $VERSION ($ARCH_LABEL)"   # 必须与 kernel/version.h 的 ZH_OS_VERSION_TEXT 一致
ISO_NAME="zh_OS-$VERSION.iso"
BUILD_DIR="$ROOT/build"
ISO_DIR="$ROOT/iso"
ELF="$BUILD_DIR/zh_OS.elf"
KERNEL_NAME_FB="zh_OS_fb.elf"           # [ADD] M25：带帧缓冲请求的同一份内核
ELF_FB="$BUILD_DIR/$KERNEL_NAME_FB"
KERNEL_NAME="zh_OS.elf"

# [ADD] M34：真机分辨率变体（各自在 multiboot2 头里请求一个固定模式）
FB1200_NAME="zh_OS_fb1200.elf"
FB800_NAME="zh_OS_fb800.elf"
FB768_NAME="zh_OS_fb768.elf"

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
  产物：zh_OS-1.0.iso
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

# [ADD] M6：编译用户态程序，并把它的 ELF 文件原样嵌进内核镜像。
#
# 两条规则都不能省：
#  1) 用户程序**不能**用内核那套 CFLAGS（-mcmodel=small / -mno-red-zone 都是给内核
#     用的），必须独立编译、独立链接，且链接到 0x40000000（M5 的进程私有窗口起点，
#     见 user/user.ld），否则装进进程后所有绝对地址都是错的。
#  2) 嵌入用 `ld -r -b binary`：它把任意文件当成"一个只含数据的 .o"，
#     生成 _binary_hello_elf_start / _end / _size 三个符号，效果等价于 Linux 的 initrd。
#     必须切到 build/ 目录执行，符号名才不含目录前缀（_binary_hello_elf_start）。
build_user_programs() {
  # [ADD] M7：用户程序清单。每加一个 user/*.c，这里加一个名字即可
  #              （hello = M6 的回归例子；forkdemo = M7 的 fork/wait 演示）。
  local progs=(hello forkdemo execdemo fddemo wrdemo spawndemo argvdemo reexec zhsh3 ls cat cowdemo demanddemo spin sigdemo sigcatch jobdemo)
  local prog

  mkdir -p "$BUILD_DIR/user"
  for prog in "${progs[@]}"; do
    info "编译用户程序 user/$prog.c ..."
    run_cmd "$CC" -m64 -ffreestanding -fno-pie -no-pie -fno-stack-protector \
      -fno-asynchronous-unwind-tables -nostdlib -O2 -Wall -Wextra \
      -c "$ROOT/user/$prog.c" -o "$BUILD_DIR/user/$prog.o"
    info "链接用户程序 $prog（装载地址 0x40000000）..."
    run_cmd "$LD" -m elf_x86_64 -T "$ROOT/user/user.ld" \
      -o "$BUILD_DIR/$prog.elf" "$BUILD_DIR/user/$prog.o"
    info "把 $prog.elf 嵌进内核镜像（ld -r -b binary）..."
    (cd "$BUILD_DIR" && "$LD" -r -b binary "$prog.elf" -o "${prog}_blob.o") || {
      die "嵌入 $prog.elf 失败"
    }
    OBJECTS+=("$BUILD_DIR/${prog}_blob.o")
    ok "用户程序已嵌入：build/$prog.elf（$(stat -c %s "$BUILD_DIR/$prog.elf") 字节）"
  done
}

# [ADD] M25：两个内核。boot.asm 里的帧缓冲请求标签用 -DFB_REQUEST 才编进来，
# 其余目标文件完全一样，只换 boot 那一个 .o。
#   $ELF      = $KERNEL_NAME      不带 fb 标签 -> GRUB 给文本模式 80x25
#   $ELF_FB   = $KERNEL_NAME_FB   带 fb 标签   -> GRUB 给图形模式 + Multiboot2 fb 标签
link_kernel() {
  local obj
  info "链接 $KERNEL_NAME ..."
  run_cmd "$LD" -m elf_x86_64 -T "$ROOT/linker.ld" -o "$ELF" "${OBJECTS[@]}"
  ok "内核已生成：$ELF"

  info "生成带帧缓冲请求的内核（-DFB_REQUEST）..."
  run_cmd nasm -f elf64 -DFB_REQUEST "$ROOT/boot/boot.asm" -o "$BUILD_DIR/boot_fb.o"
  local objs_fb=()
  for obj in "${OBJECTS[@]}"; do
    if [[ "$obj" == "$BUILD_DIR/boot/boot.o" ]]; then
      objs_fb+=("$BUILD_DIR/boot_fb.o")
    else
      objs_fb+=("$obj")
    fi
  done
  run_cmd "$LD" -m elf_x86_64 -T "$ROOT/linker.ld" -o "$ELF_FB" "${objs_fb[@]}"
  ok "图形模式内核已生成：$ELF_FB"

  # [ADD] M34：真机改分辨率。GRUB 只认 multiboot2 头里写的帧缓冲请求，
  # gfxpayload 在带请求标签时会被忽略（实测），所以每个目标分辨率编一个内核，
  # 菜单里选哪个就加载哪个。
  link_fb_variant 1920 1200 "$FB1200_NAME"
  link_fb_variant 1280 800  "$FB800_NAME"
  link_fb_variant 1024 768  "$FB768_NAME"
}

# [ADD] M34：编一个"明确请求 WxHx32 帧缓冲"的内核变体。
link_fb_variant() {
  local w="$1" h="$2" name="$3" obj
  local obj_file="$BUILD_DIR/boot_${name}.o"
  local objs_v=()

  info "生成 $name（${w}x${h}x32）..."
  run_cmd nasm -f elf64 -DFB_REQUEST -DFB_W="$w" -DFB_H="$h" -DFB_D=32 \
    "$ROOT/boot/boot.asm" -o "$obj_file"
  for obj in "${OBJECTS[@]}"; do
    if [[ "$obj" == "$BUILD_DIR/boot/boot.o" ]]; then
      objs_v+=("$obj_file")
    else
      objs_v+=("$obj")
    fi
  done
  run_cmd "$LD" -m elf_x86_64 -T "$ROOT/linker.ld" -o "$BUILD_DIR/$name" "${objs_v[@]}"
  ok "已生成：build/$name（${w}x${h}x32）"
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
set timeout=3
set default=0

# [ADD] M25：分辨率。GRUB 用 gfxpayload 控制交给内核的图形模式，
# multiboot2 会把 framebuffer 标签（type 8）一起传下去，内核据此算行列。
insmod vbe
insmod gfxterm
set gfxmode=auto
terminal_output gfxterm
set gfxpayload=keep

# 默认进文本模式：这样老的分等效验证脚本（靠读 0xB8000）都还能跑。
# 想要图形模式在第 2 项，按一下方向键下再回车即可（res 命令只在图形模式下有效）。
menuentry "$VERSION_TEXT  text mode 80x25 (no framebuffer)" {
    set gfxpayload=text
    multiboot2 /boot/$KERNEL_NAME
    boot
}
menuentry "$VERSION_TEXT  graphics (framebuffer, res switchable)" {
    multiboot2 /boot/$KERNEL_NAME_FB
    boot
}
# [ADD] M34：真机改分辨率。
#
# 为什么不在这里写 set gfxmode / set gfxpayload：只要内核的 multiboot2 头里带了
# 帧缓冲请求标签（type 5），GRUB 就按"请求里写的宽高"去要模式，gfxpayload 会被
# 忽略（实测：写 1920x1200x32 内核照样拿到 1280x800）。所以每个目标分辨率单独
# 编一个内核变体（头里写死 1920x1200/1280x800/1024x768 + 32bpp），菜单项直接
# 加载对应的那个 ELF —— 简单、确定、不依赖 GRUB 的模式选择策略。
menuentry "$VERSION_TEXT  graphics  1920x1200x32" {
    multiboot2 /boot/$FB1200_NAME
    boot
}
menuentry "$VERSION_TEXT  graphics  1280x800x32" {
    multiboot2 /boot/$FB800_NAME
    boot
}
menuentry "$VERSION_TEXT  graphics  1024x768x32" {
    multiboot2 /boot/$FB768_NAME
    boot
}
EOF
}

build_iso() {
  info "组织 ISO 目录 ..."
  rm -rf "$ISO_DIR"
  mkdir -p "$ISO_DIR/boot/grub"
  cp "$ELF" "$ISO_DIR/boot/$KERNEL_NAME"
  cp "$ELF_FB" "$ISO_DIR/boot/$KERNEL_NAME_FB"
  cp "$BUILD_DIR/$FB1200_NAME" "$ISO_DIR/boot/$FB1200_NAME"
  cp "$BUILD_DIR/$FB800_NAME"  "$ISO_DIR/boot/$FB800_NAME"
  cp "$BUILD_DIR/$FB768_NAME"  "$ISO_DIR/boot/$FB768_NAME"
  write_grub_cfg

  build_efi_image

  info "生成 $ISO_NAME（BIOS + UEFI 双启动）..."
  # [ADD] M29：手工拼 ISO，一张盘同时带 El Torito(BIOS) 与 El Torito(EFI)。
  # grub-mkrescue 只会按本机装的平台出图，这里两个都自己装。
  mkdir -p "$ISO_DIR/boot/grub/i386-pc"
  cp -f /usr/lib/grub/i386-pc/*.mod "$ISO_DIR/boot/grub/i386-pc/" 2>/dev/null || true
  cp -f /usr/lib/grub/i386-pc/*.lst "$ISO_DIR/boot/grub/i386-pc/" 2>/dev/null || true
  cp -f /usr/lib/grub/i386-pc/eltorito.img "$ISO_DIR/boot/grub/i386-pc/" 2>/dev/null || true
  run_cmd xorriso -as mkisofs -quiet -volid ZH_OS -o "$ROOT/$ISO_NAME" \
    -b boot/grub/i386-pc/eltorito.img -no-emul-boot -boot-load-size 4 \
    -boot-info-table --grub2-boot-info \
    --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img \
    -eltorito-alt-boot -e EFI/efi.img -no-emul-boot \
    -isohybrid-gpt-basdat \
    "$ISO_DIR"
}

# [ADD] M29：UEFI 引导镜像。
# 思路：BOOTX64.EFI 用 grub-mkstandalone 做成"自带 grub.cfg + 必需模块"的单文件，
# 塞进一张 8MB 的 FAT 镜像（EFI/efi.img），xorriso 再把它当第二个 El Torito 入口。
# 这样 BIOS 走 eltorito.img、UEFI 走 BOOTX64.EFI，同一张盘两种固件都能起。
build_efi_image() {
  local efi_mods="$ROOT/build/grub-efi"
  local efi_cfg="$ROOT/build/efi_grub.cfg"
  local bootx="$ROOT/build/BOOTX64.EFI"
  local efi_img="$ROOT/build/efi.img"

  if [ ! -d "$efi_mods" ]; then
    warn "缺少 $efi_mods（x86_64-efi 模块），本次只做 BIOS 引导"
    warn "修复：bash build/get_grub_efi.sh"
    mkdir -p "$ISO_DIR/EFI"
    dd if=/dev/zero of="$ISO_DIR/EFI/efi.img" bs=1M count=1 status=none
    return 0
  fi

  info "生成 EFI 引导镜像 ..."
  write_efi_grub_cfg "$efi_cfg"
  # 内核也塞进 memdisk：EFI 侧 GRUB 的 iso9660 常常扫不到光驱，
  # 干脆让它不依赖光驱 —— 镜像本身就是内核的载体。
  run_cmd grub-mkstandalone -d "$efi_mods" -O x86_64-efi \
    --fonts="" --locales="" --themes="" \
    --modules="multiboot2 normal iso9660 part_msdos part_gpt video all_video gfxterm boot" \
    -o "$bootx" "boot/grub/grub.cfg=$efi_cfg" \
    "boot/$KERNEL_NAME_FB=$ELF_FB" \
    "boot/$FB1200_NAME=$BUILD_DIR/$FB1200_NAME" \
    "boot/$FB800_NAME=$BUILD_DIR/$FB800_NAME" \
    "boot/$FB768_NAME=$BUILD_DIR/$FB768_NAME" \
    "boot/$KERNEL_NAME=$ELF"

  rm -f "$efi_img"
  dd if=/dev/zero of="$efi_img" bs=1M count=12 status=none
  mformat -i "$efi_img" -M 512 ::
  mmd -i "$efi_img" ::/EFI ::/EFI/BOOT
  mcopy -i "$efi_img" "$bootx" ::/EFI/BOOT/BOOTX64.EFI
  mkdir -p "$ISO_DIR/EFI"
  cp -f "$efi_img" "$ISO_DIR/EFI/efi.img"
}

# [ADD] M29：UEFI 下没有 VGA 文本模式，只留图形模式这一项，
# 并且 gfxmode=auto 让固件挑它自己的原生分辨率（GOP）。
write_efi_grub_cfg() {
  local out="$1"
  cat > "$out" <<EOF
set timeout=3
set default=0
insmod all_video
insmod gfxterm
set gfxmode=auto
terminal_output gfxterm
set gfxpayload=keep
set root=(memdisk)
menuentry "$VERSION_TEXT  UEFI graphics (framebuffer)" {
    multiboot2 /boot/$KERNEL_NAME_FB
    boot
}
# [ADD] M34：真机（UEFI/GOP）分辨率 —— 同样靠内核变体，理由见 BIOS 那份注释。
menuentry "$VERSION_TEXT  UEFI graphics  1920x1200x32" {
    multiboot2 /boot/$FB1200_NAME
    boot
}
menuentry "$VERSION_TEXT  UEFI graphics  1280x800x32" {
    multiboot2 /boot/$FB800_NAME
    boot
}
menuentry "$VERSION_TEXT  UEFI graphics  1024x768x32" {
    multiboot2 /boot/$FB768_NAME
    boot
}
EOF
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
  SOURCES+=("$ROOT/fs/initrd.c")  # [ADD] M8：把嵌入的 ELF 铺进 ramfs（fs/ 同样不在自动扫描列表）
  SOURCES+=("$ROOT/fs/file.c")  # [ADD] M9：每进程 fd 表
  SOURCES+=("$ROOT/fs/ext2.c")  # [ADD] M19：ext2 只读（fs/ 不在自动扫描列表，显式登记）
  SOURCES+=("$ROOT/fs/vfs.c")   # [ADD] M20：挂载表 + 最长前缀路径解析
  # [ADD] drivers/ata.c 与 users/users_persist.c 不需要显式登记：collect_sources 的目录列表
  #       已含 drivers/ 与 users/，会自动扫描到这两个文件。若再 SOURCES+= 同路径，链接时会
  #       出现 "multiple definition"（同一 .o 被当两次输入）。故此处仅留说明，不追加重复路径。
  #       （与 arch/、fs/ 不同：那两个是新目录，不在自动扫描列表里，才必须显式登记。）
  compile_sources
  build_user_programs   # [ADD] M6：用户态 ELF + 二进制嵌入
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
