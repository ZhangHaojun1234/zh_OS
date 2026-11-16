zh_OS 0.2beta

从 0.1beta 的"32 位、21 条命令"一路推到"64 位、58 条命令、能跑用户态进程和文件系统"。

## 主要变化

### 架构
- 32 位 → **64 位长模式**：两阶段引导（实模式 → 32 位保护模式 → 4 级页表 → long mode），
  `boot/boot.asm` 一条龙完成，Multiboot2 头带帧缓冲请求标签（`-DFB_REQUEST` 才编进去）。
- 独立的 `kernel/paging.c`：低 1GB 用 2MB 大页恒等映射，设备内存（LFB）另用
  `paging_map_2mb_range()` 现建页表，写完 `invlpg`。
- 每进程独立 CR3 + 内核栈 + TSS，`syscall` 走 `int 0x80`（保留 Linux 风格的编号表）。

### 进程与用户态
- 多任务调度（PIT 抢占 + 就绪队列），`fork` / `exec` / `wait` / `exit` 全链路。
- fork 用 **COW**（写时复制）：父子共享物理页，写触发 #PF 才复制。
- `.bss` 按需映射，`demanddemo` 能看到页错误的次数。
- 信号：排队投递 + 用户态 handler + `sigreturn` 恢复现场。
- 用户程序是真 ELF64（`gcc -nostdlib` 编、链到 0x40000000），由内核 ELF loader 装载；
  `shell 本身的 zhsh3` 就跑在 ring 3。
- 独立 ring3 程序：`/bin/ls.elf`、`/bin/cat.elf`（走 `SYS_READDIR` 一条条读目录）。

### 文件系统
- `ramfs`（内存 FS）、`FAT16` 只读（带镜像）、`ext2` **读写**：超级块/组描述符/inode 表/
  目录项/直接块 + 一级间接块全实现，`e2fsck -fn` 校验零告警。
- `VFS` 统一命名空间：最长前缀匹配挂载表，`mount/umount` 后 `ls/cd/cat/touch/rm` 全走 VFS，
  用户态代码一行不用改。
- 每进程 fd 表 + `initrd` 把嵌入的 ELF 铺进 `/bin`。

### 显示（本次重点新增）
- **图形模式帧缓冲控制台**：GRUB 给 Multiboot2 fb 标签，内核自己用 8×16 点阵字体
  逐像素画字，行列按分辨率算（`列 = 宽/8`、`行 = 高/16`），和 Linux fbcon 同构。
- `videomode`：报当前 `framebuffer WxHxBpp pitch` 与 `text grid 列x行`。
- `res <W>x<H> [bpp]`：**运行时换分辨率**（Bochs VBE dispi，端口 0x1CE/0x1CF，
  先验 ID 0xB0C0..0xB0C5，真机没有这套接口就红字拒绝）。分隔符 `x` / `X` / `*` 都认。
- **整屏字符缓冲**：换模式不再丢屏 —— 旧屏幕"真正用过的最后几行"搬到新屏幕底部、
  按新几何整屏重绘，光标贴底继续滚（和 Linux fbcon 换模式行为一致）。
- 两个内核：`zh_OS.elf`（文本模式 80×25，老验收脚本靠它读 0xB8000）与
  `zh_OS_fb.elf`（图形模式），GRUB 菜单两项。

### 命令表
- 21 条 → **58 条**（含 `videomode` / `res`），`help` 彩色列表，`COMMANDS.txt` 逐条详解。

## 验收

```
bash build/regress_all.sh     # M2 / M4 / M5 / M6 ... M25 / M26
98 行 PASS，0 个 NO MATCH
```

每个里程碑都有对应的 `build/qemu_verify_mXX.sh` 抓屏脚本与截图，判据是屏上文字/PPM 尺寸，
不是"看起来跑了"。

## 下载

- `zh_OS-0.2beta.iso` — 直接用 QEMU 启动

```bash
qemu-system-x86_64 -cdrom zh_OS-0.2beta.iso -m 128
```

GRUB 菜单：第 1 项文本模式（默认），第 2 项图形模式（`res` 在这边才有意义）。
登录：`zh / zh`（root 也是 `zh`）。首次登录后建议 `passwd` 改密码。

## 已知限制

- 换分辨率依赖 Bochs/QEMU 的 dispi 接口，真机需要 VBE BIOS 调用或 GPU 驱动（还没有）。
- 字符缓冲只覆盖可见一屏，没有 scrollback，也没有多虚拟终端。
- ext2 只支持一级间接块（单文件上限约 268KB），无二级/三级间接。
- 单核、无 SMP，无抢占优先级，无网络。
