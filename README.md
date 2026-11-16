# zh_OS 0.2beta (x86_64)

zh_OS 是一个教学用的 x86_64 64 位微型内核：GRUB2 + Multiboot2 引导，NASM + freestanding C
（无 libc），运行在 80x25 VGA 彩色文本模式下。它已经具备长模式、4 级页表、GDT、IDT/ISR/PIC、
PS/2 键盘、行编辑器、仿 Linux 的账户登录、带彩色提示符的内置 Shell、内存文件系统 ramfs、
ATA PIO 磁盘驱动（账户表持久化）以及 FAT16 只读挂载。

- **统一版本号**：`zh_OS 0.2beta (x86_64)`。启动横幅、`uname` 命令、登录横幅与 GRUB 菜单项
  都由 `kernel/version.h` 的 `ZH_OS_VERSION_TEXT` 提供；`build.sh` 在构建时会校验脚本侧的
  `VERSION_TEXT` 与它一致，防止只改一处造成版本漂移。
- **开发/构建环境**：宿主机 Windows 11，构建环境 WSL2 + Ubuntu。所有脚本都是 Linux `.sh`，
  在 WSL 内运行，**不生成任何 `.bat`**。
- **可启动产物**：`zh_OS-0.2beta.iso`（Hybrid ISO，可用于 QEMU / VirtualBox / VMware）。

> **首次登录**：内置 `root`（uid 0 / gid 0 / `/root` / 管理员）与
> `zh`（uid 1000 / gid 1000 / `/home/zh`），**两者初始密码都是 `zh`**。
> `useradd` 新建的账户初始密码同样是 `zh`。
>
> **账户持久化**：默认走内存态，重启即复位。若启动时挂了一块可写磁盘
> （`-drive file=disk.img,format=raw,if=ide`），内核会在登录前从 `LBA 200-207`
> 读回账户表，`useradd` / `passwd` 成功后立即写回，账户与密码即可跨重启保留。
> 只从 CD 启动时没有可写目标，`useradd` / `passwd` 会多打一行黄字
> `[warn] disk read-only, changes lost on reboot`。

## 目录结构

```
zh_OS/
├── boot/           引导代码（Multiboot2 头、32->64 位两段式入口、临时页表）
├── kernel/         内核核心（GDT、页表、IDT、中断分发、PIC、控制台、主入口、版本头）
│   ├── version.h   全项目唯一的版本号宏（ZH_OS_VERSION_TEXT）
│   ├── tss.c       64 位 TSS + 重建 7 项 GDT + ltr（M1 ring3 的前提）
│   ├── ring3.asm   进 ring3 保存内核现场 / usermode_resume 回内核（M2）
│   ├── usermode.c  ring3 自检：拼机器码、页表 U/S 置位、iretq 降权（M1）
│   ├── syscall.c   int 0x80 门、sys_write/ticks/exit/cpl（M2）、putc/sleep（M4）、fork/wait（M7）、exec（M8）
│   ├── task.c      M4 进程与调度：TCB、引导帧、时钟抢占、环形轮转、异常杀任务
│   ├── task_switch.asm  M4 zh_sched_enter / zh_resume_frame / zh_sched_leave
│   ├── vmm.c       M5 进程地址空间：每进程 PML4/PDPT/PD/PT、4KB 页、独立 CR3
│   ├── elf.c       M6 ELF64 装载器：校验头、按 PT_LOAD 逐段映射并拷贝、取 e_entry
│   ├── klog.c      内核日志环（128 槽，dmesg 数据源）
│   └── history.c   Shell 历史（上/下箭头召回）
├── drivers/        设备驱动（vga、keyboard、pit、ata、fb）
│   ├── ata.c       ATA PIO：LBA28 读写主/从盘，LBA 200 存取用户表
│   ├── fb.c        M25 线性帧缓冲控制台：Multiboot2 标签解析 + 清屏/写字/滚屏 + res 换模式
│   ├── font8x16.c  M25 8×16 点阵字体（95 个 ASCII 字形，build/mkfont.py 生成）
│   └── font8x16.h  字体尺寸常量与 extern 声明
├── users/          账户与登录（users.c/.h、login.c/.h、users_persist.c/.h）
├── shell/          Shell（shell.c/.h、commands.c 命令表、cmd_*.c 各命令实现）
├── fs/             文件系统（ramfs.c 内存 FS、fat16.c FAT16 只读、ext2.c ext2 只读、vfs.c 统一命名空间挂载表、initrd.c 把嵌入的 ELF 铺进 /bin、file.c 每进程 fd 表）
├── user/           用户态程序（hello.c、forkdemo.c、execdemo.c、fddemo.c、wrdemo.c、spawndemo.c、argvdemo.c、reexec.c、zhsh3.c、ls.c、cat.c、cowdemo.c、demanddemo.c、spin.c、sigdemo.c、sigcatch.c、jobdemo.c：用 gcc 编成真 ELF64；user.ld：链接到 0x40000000）
├── mm/             内核堆（heap.c：2MB 池的 first-fit + 相邻块合并）
│   └── phys.c      M3 物理内存：Multiboot2 内存图 + 4KB 页帧位图分配器
├── arch/           CPU 信息采集（cpuinfo.c，CPUID 封装）
├── docs/           历史截图（第 2 步的彩色横幅、调色板自检）
├── build/          中间产物：.o、内核 ELF、QEMU 屏取与可复用验证脚本
├── iso/            ISO 组织目录（boot/zh_OS.elf 文本、boot/zh_OS_fb.elf 图形、boot/grub/grub.cfg）
├── linker.ld       链接脚本（加载地址 0x100000，.bss 4K 对齐）
├── setup.sh        环境配置（安装依赖、可选交叉工具链、写 env.mode）
├── check.sh        只读环境体检：打印 gcc/nasm/grub-mkrescue/xorriso/qemu 版本
├── build.sh        一键编译（内核 + 用户态 ELF + 二进制嵌入）+ grub-mkrescue 出 ISO
├── env.mode        工具链路线（system-gcc / cross）
├── COMMANDS.txt    58 条内置命令速查与详解
└── README.md
```

## 一、WSL2 Ubuntu 环境准备（Windows 11）

1. 以**管理员身份**打开 PowerShell，安装 WSL2 与 Ubuntu：

   ```powershell
   wsl --install -d Ubuntu
   ```

   若提示需要重启，重启后按引导设置 Ubuntu 的用户名与密码。确认版本为 WSL2：

   ```powershell
   wsl -l -v
   ```

2. 进入 Ubuntu，把项目放到家目录（项目原本在 Windows 桌面时）：

   ```bash
   sudo apt update && sudo apt upgrade -y
   cp -r /mnt/c/Users/你的用户名/Desktop/zh_OS ~/zh_OS
   cd ~/zh_OS
   ```

   > 如果不复制到家目录，也可以直接在 Windows 原路径工作：
   > `cd /mnt/c/Users/你的用户名/Desktop/zh_OS`（跨文件系统编译会稍慢）。

3. 赋予脚本执行权限并初始化环境：

   ```bash
   chmod +x setup.sh check.sh build.sh
   ./setup.sh
   ./check.sh
   ```

   `setup.sh` 会安装 build-essential、nasm、grub、xorriso、mtools、qemu，并把
   `env.mode` 写成 `system-gcc`。需要源码编译 `x86_64-elf` 交叉工具链时用
   `./setup.sh --build-cross-toolchain`（耗时 20-60 分钟）。

   `check.sh` 只读检测、不安装任何东西，会打印
   `gcc / nasm / grub-mkrescue / xorriso / qemu-system-x86_64` 的版本及修复建议，
   排错时先跑它。

## 二、构建

```bash
cd ~/zh_OS
./build.sh
```

| 产物 | 说明 |
| --- | --- |
| `zh_OS-0.2beta.iso` | 可启动 ISO，供 QEMU / VirtualBox / VMware 使用 |
| `build/zh_OS.elf` | 内核 ELF（调试用） |
| `iso/` | ISO 组织目录（`iso/boot/zh_OS.elf`、`iso/boot/grub/grub.cfg`） |

`build.sh` 读取 `env.mode` 决定工具链（`system-gcc` 用系统 gcc/ld，`cross` 用
`toolchain/bin/x86_64-elf-gcc`）。它会自动扫描
`kernel/ boot/ drivers/ mm/ shell/ users/` **顶层**的所有 `.c` / `.asm`，
头文件搜索路径为 `-I kernel -I drivers -I users -I shell`，
**这些目录不需要手工维护编译列表**。

`arch/` 与 `fs/` **不在**自动扫描列表内，新文件必须显式追加一行
`SOURCES+=("$ROOT/fs/xxx.c")  # [ADD]`（`arch/cpuinfo.c`、`fs/ramfs.c`、
`fs/fat16.c`、`fs/ext2.c` 就是这样登记的）。反过来，给 `drivers/` 或 `users/` 已自动扫描的
文件再写 `SOURCES+=` 会让同一个 `.o` 被链接两次，报 multiple definition。

关键编译选项（缺一不可）：

```
-m64 -ffreestanding -fno-pie -mno-red-zone -mcmodel=small
-Wall -Wextra -fno-stack-protector -nostdlib
```

可选开关：`ZH_VGA_SELFTEST=1 ./build.sh` 编入启动时的 16 色前景自检（默认关闭）。

> **改动任何源码后都要重新执行 `./build.sh` 生成新 ISO**，旧 ISO 不会自动更新。
> 可用 `strings build/zh_OS.elf | grep -E 'Welcome back|built-in commands|command not found'`
> 确认新代码确实编进去了。

## 三、常见报错与修复

**`未找到 env.mode` / `env.mode 为空`**

先运行 `./setup.sh`；它会写入 `system-gcc`。也可手动：

```bash
echo system-gcc > env.mode
```

**`未找到 grub-mkrescue` / `grub-mkrescue: command not found`**

`grub-mkrescue` 由 `grub-pc-bin` 提供（不是 `grub2` 包），且生成 ISO 还需要 xorriso/mtools：

```bash
sudo apt-get update
sudo apt-get install -y grub-pc-bin grub-common xorriso mtools
```

**漏 `-mno-red-zone` 导致随机崩溃 / 三重异常**

x86-64 SysV ABI 允许函数在 `rsp` 下方 128 字节的 "red zone" 存临时数据而不调整栈指针；
但 CPU 发生中断/异常时会无条件把中断帧压到当前 `rsp` 之下，直接覆盖这片区域。
内核代码运行在 ring 0、随时可能被中断，因此必须加 `-mno-red-zone`。
漏掉时症状是"看起来能跑，一开中断就随机死"，且不报错，最难排查。

**漏 `-fno-stack-protector` 导致 `__stack_chk_fail` 未定义**

GCC 默认会插入栈保护代码并调用 libc 的 `__stack_chk_fail`；freestanding 内核没有 libc，
链接阶段会报 `undefined reference to '__stack_chk_fail'`。加 `-fno-stack-protector`
（本项目已加）即可。

**`nasm: command not found` / `undefined reference to '_start'`**

```bash
sudo apt-get install -y nasm
```

后者通常是 `boot/boot.asm` 没被编入（检查是否放在被扫描的目录顶层）或链接脚本入口写错。

**屏幕上的中文变成乱码**

VGA 文本模式是 CP437 字符集，没有中文字模。内核里的提示/日志必须用 ASCII；
中文只保留在注释、README 与终端脚本输出中。

## 四、首次启动与登录

1. GRUB 菜单显示 **zh_OS 0.2beta (x86_64)**，自动进入内核。
2. 短暂显示 Linux printk 风格启动日志（banner、`gdt:` / `paging:` / `vga:` / `idt:` /
   `pic:` / `keyboard:` / `users:`，亮绿 `Long mode active.` 与 `Interrupts enabled (sti).`）。
   末尾还有一行蓝字 ATA 探测结果：挂了可写盘是 `[ATA] user table loaded from disk`，
   没挂盘是 `[ATA] no disk, using defaults`。
3. `login_screen()` 清屏，画蓝框 + 三色标题 `zh_OS 0.2beta (x86_64) Login`。
4. `login: ` 输入 `zh`，`Password: ` 输入 `zh`（回显 `*`），得到亮绿
   `Welcome back, zh!` 与 `uid=1000 gid=1000 home=/home/zh shell=/bin/zhsh`。
5. 进入彩色 Shell 提示符 `[zh@zh_OS ~]$ `（root 为红色 `#`）。

连续输错 3 次密码会给出黄字重启提示，按回车后清屏重来；不存在用户与密码错误统一提示
`Login incorrect.`，不泄露账户名是否存在。

## 五、命令清单

| 命令 | 作用 | 权限 |
| --- | --- | --- |
| `help` | 彩色列出全部命令与说明 | 全部用户 |
| `clear` | 清屏 | 全部用户 |
| `echo [args...]` | 绿色输出参数（空格连接） | 全部用户 |
| `uname` | 亮绿输出 `zh_OS 0.2beta (x86_64)` | 全部用户 |
| `whoami` | 输出当前用户名 | 全部用户 |
| `login` | 在当前 Shell 内重新登录，返回后提示符按新用户上色 | 全部用户 |
| `logout` | 退出 Shell，回到登录界面 | 全部用户 |
| `su [user]` | 切换用户（缺省 `root`），需输入目标账户密码 | 知道目标密码即可 |
| `useradd <name>` | 新建账户，uid/gid 递增，初始密码 `zh` | **仅 root** |
| `passwd [user]` | 改自己密码；root 可用 `passwd <user>` 改他人 | 普通用户仅能改自己 |
| `color` | 8 行 × 2 列一次展示 16 种前景色 | 全部用户 |
| `reboot` | 通过 8042 键盘控制器 `0xFE` 复位 CPU | 全部用户 |
| `shutdown` | 打印 `System halted.` 后 `cli; hlt` 停机（无 ACPI 断电） | 全部用户 |
| `meminfo [test]` | 显示内核堆用量（pool/used/free/blocks）；`test` 跑分配/合并/越界自测 | 全部用户 |
| `uptime` | 显示开机时长 `up X hours, Y minutes, Z seconds` | 全部用户 |
| `date` | 显示 RTC 日期与时间 `YYYY-MM-DD HH:MM:SS` | 全部用户 |
| `dmesg [test]` | 显示内核日志环（启动/运行日志，带 tick）；`test` 灌满环验证覆盖 | 全部用户 |
| `cpuinfo` | 显示 CPU 厂商、型号（Brand String）、Family/Model/Stepping、特性与逻辑处理器数 | 全部用户 |
| `ls [path]` | ramfs 列目录：目录亮蓝带 `/`、文件浅灰带大小，每行 4 个 | 全部用户 |
| `cd <path>` | 切换 ramfs 当前目录（提示符路径段随之变化） | 全部用户 |
| `pwd` | 打印当前目录绝对路径 | 全部用户 |
| `cat <file>` | 打印 ramfs 文件内容，>4KB 截断并黄字提示 | 全部用户 |
| `mkdir <path>` | 新建 ramfs 目录 | root 或自己 home 子树内 |
| `touch <path>` | 新建空文件 | root 或自己 home 子树内 |
| `rm <path>` | 删除文件或空目录 | root 或自己 home 子树内 |
| `diskinfo` | 显示 ATA 主盘是否存在、账户表持久化扇区范围 | 全部用户 |
| `fatmount` | 挂载主通道第二块盘上的 FAT16（只读） | 全部用户 |
| `fatls [path]` | 列 FAT16 目录（根目录或一层子目录） | 全部用户 |
| `fatcat <path>` | 打印 FAT16 文件，>4KB 截断并黄字提示 | 全部用户 |
| `ring3` | 进 ring 3 自检：合法写入 + 被 PIT 打断 + 越权写 VGA 触发 #PF（M1） | 全部用户 |
| `syscall` | ring3 经 `int 0x80` 调内核（write/ticks/exit/cpl）再返回 Shell（M2） | 全部用户 |
| `memmap [test]` | 显示 Multiboot2 内存图 + 4KB 页帧分配器；`test` 跑分配/释放自测（M3） | 全部用户 |
| `tasks` | 打印 TCB 表：状态、累计 tick、换入次数、退出码或被杀原因（M4） | 全部用户 |
| `taskrun [fault\|dry]` | 跑两个 ring3 进程，看 A/B 被时钟抢占着交替打印并切换 CR3；`fault` 让 B 踩未映射页被杀；`dry` 只建进程打印页表（M4/M5） | 全部用户 |
| `vmspace` | 打印每个进程的 PML4/PDPT/PD/PT 物理地址、已映射的 4KB 页（VA→PA）、CR3（M5） | 全部用户 |
| `elf` | 解析内嵌的 `hello.elf`、按 PT_LOAD 装进新进程的地址空间、用 `e_entry` 进 ring3 跑完并校验退出码（M6）；`elf parse` 只解析不装载；`elf fork` 跑 fork/wait 演示（M7）；`elf exec` 从 ramfs 按路径装载并让进程 exec 掉自己（M8） | 全部用户 |
| `fd` | 装载 `/bin/fddemo.elf`，让 ring3 程序自己 `open`/`read`/`close` 一个 ELF（M9）：fd 是每进程 fd 表的下标，读偏移随 read 前移，close 后失效 | 全部用户 |
| `wr` | 装载 `/bin/wrdemo.elf`：ring3 程序 `creat` 一个文件、向同一个 fd 追加两段、`close` 后再只读 `open` 读回自己的字节（M10） | 全部用户 |
| `spawn` | 装载 `/bin/spawndemo.elf`：`fork` → 子进程 `exec` 成 `/bin/hello.elf` → 父进程 `wait` 收到 42（M10，等于 shell 执行一条外部命令的全部内核动作） | 全部用户 |
| `argv` | 装载 `/bin/argvdemo.elf`：父进程 `fork` → 子进程 `exec` **同一个程序**但带上 `{"argvdemo","alpha","beta"}` → 新映像的 `_start(argc, argv)` 打印出 3 个参数、exit 0（M11，Linux `execve` 的 argv 部分） | 全部用户 |
| `mm` | 装载 `/bin/reexec.elf`：它连续 exec 自己 7 次（每次一套新 mm），用 argv 传剩余深度；命令对比前后页帧空闲数，证明旧 mm 每次都被销毁（M12，Linux `mmput()` 的雏形） | 全部用户 |
| `sh3` | 启动 `/bin/zhsh3.elf` —— **一个跑在 ring 3 的 shell**：它靠 `SYS_READKEY` 一个键一个键地读、自己回显、自己拆 argv，外部命令一律 `fork` → `exec` → `wait`（M13） | 全部用户 |
| `cow` | 跑 `/bin/cowdemo.elf`：父子各写一次同一页，命令行打印 `shared / copied / faults`，证明 fork **只共享不复制**、只有写过的页才复制（M15） | 全部用户 |
| `demand` | 跑 `/bin/demanddemo.elf`：它声明 64KB `.bss`（16 页）却只碰 3 页，命令打印 `cost@load` / `cost@run` / `pages materialised`，证明页在**第一次访问时才出现**（M16） | 全部用户 |
| `sig` | 投递一个 SIGKILL 给还没跑的自旋进程 + 跑 `/bin/sigdemo.elf`（fork→kill→waitpid），打印 `term_sig` / `exit_code=137`，证明信号在**回 ring3 那一刻**才送达（M17） | 全部用户 |

合计 **58 条**，与 `help` 输出条数一致。其中 `ls/cd/pwd/cat/mkdir/touch/rm`
操作的是 **ramfs**（内存文件系统），`fatls/fatcat` 操作的是 **FAT16 只读盘**，
`ring3/syscall` 是用户态自检，`memmap` 看物理内存，`tasks/taskrun` 看进程与调度，
`vmspace` 看每个进程自己的页表，互不覆盖、互不干扰。

未知命令：红色 `zh_OS: command not found: <cmd>` + 黄字 `Type 'help' for a list of commands.`
权限不足：红色 `Permission denied.`。
`help / color / useradd / su / logout` 等彩色输出由 `shell/shell.h` 的
`err / ok / warn / info / log / plain` 六个输出宏统一上色。

> **配色（Linux 红 / 黄 / 绿三色分级）**：红 = 错误、权限拒绝、root 提示符；
> 黄 = 警告、标签、主机名、日期；绿 = 成功、普通用户、路径、正常数值与信息标题；
> 其余文字用默认浅灰 / 白。`color` 命令仍展示完整 16 色调色板（该命令本身就是颜色演示）。
> 映射集中在 `shell/shell.c` 的 `level_color()`，改一处即可全局生效。

## 六、测试步骤

### 6.1 WSL 内 QEMU（推荐）

```bash
cd ~/zh_OS
qemu-system-x86_64 -cdrom zh_OS-0.2beta.iso -m 128
```

关闭 QEMU：按 `Ctrl+Alt+G` 释放鼠标后关闭窗口，或在终端按 `Ctrl+C`。

需要验证磁盘功能时，再按需挂两块 IDE 盘（主盘存账户表，从盘放 FAT16 数据）：

```bash
# 主盘：16MB 全零文件即可，内核用它存 LBA 200-207 的账户表
dd if=/dev/zero of=disk.img bs=1M count=16

# 从盘：脚本用 mkfs.vfat -F 16 造 32MB 盘，并写入 hello.txt / zh.txt / sub/
./build/mkfat16_img.sh

qemu-system-x86_64 -cdrom zh_OS-0.2beta.iso -m 128 \
  -drive file=disk.img,format=raw,if=ide \
  -drive file=data.img,format=raw,if=ide,index=1
```

> WSL 若没装 `dosfstools`（`mkfs.vfat` 不存在）且 `sudo` 需要交互密码，
> `mkfat16_img.sh` 会自动 `apt-get download dosfstools` 再 `dpkg-deb -x`
> 解到 `build/fattools/` 直接调用，不需要 root、不改动系统。

无头回归验证（无需 X，用 HMP monitor 注入按键并截屏）可复用 `build/` 下的脚本：

```bash
bash build/qemu_verify_step5.sh        # 登录 -> help -> color -> useradd -> su root -> logout -> test
bash build/qemu_verify_extra.sh        # 未知命令 / echo / uname / whoami / passwd / shutdown
bash build/qemu_verify_reboot.sh       # reboot 后重新引导到登录界面
bash build/qemu_dump_ramfs.sh          # ramfs 七命令逐屏截图
bash build/qemu_dump_ata.sh no         # 无盘：diskinfo 黄字 + useradd 只读警告
bash build/qemu_dump_ata.sh hdd        # 有盘：useradd 持久化，重启后仍可登录
bash build/qemu_dump_ata_passwd.sh     # 改密码跨重启生效
bash build/qemu_verify_ata_regress.sh  # cpuinfo / ramfs / 历史键 / logout 回归
bash build/qemu_dump_fat.sh            # FAT16 十屏验收（mount/list/cat/子目录/ramfs 不受影响）
bash build/qemu_verify_fat_regress.sh  # 全量回归（ramfs + 时间 + cpuinfo + dmesg + diskinfo + FAT）
python3 build/check_step5.py           # Windows/WSL 侧静态检查（宏、符号、ASCII、命令表）
python3 build/parse_vga.py dump.log 'a,b,c'   # 把 xp 导出的 VGA 文本渲染成 80x25 屏幕
```

### 6.2 Windows 下 VirtualBox

先把 ISO 复制到 Windows（在 WSL 内执行，把用户名换成实际的）：

```bash
cp zh_OS-0.2beta.iso /mnt/c/Users/你的用户名/Desktop/
```

然后在 VirtualBox 中：

1. 新建虚拟机 → 类型 **Linux**，版本 **Other / Unknown 64-bit**
2. 内存 **128 MB** 以上，不创建虚拟硬盘
3. 设置 → 存储 → 光驱选择桌面上的 `zh_OS-0.2beta.iso`
4. 启动，GRUB 菜单出现后回车

### 6.3 Windows 下 VMware Workstation

同样先把 ISO 复制到 Windows 桌面（命令同上），然后：

1. 新建虚拟机 → 典型（或自定义）→ 稍后安装操作系统
2. 客户机操作系统选 **Linux**，版本 **其他 Linux 2.6 x86_64**
   （Other Linux 2.6.x kernel 64-bit）
3. 内存 128 MB 以上，光驱（CD/DVD）加载 `zh_OS-0.2beta.iso`
4. 启动虚拟机

> VirtualBox / VMware 都是 Windows 图形程序，需要在宿主机手动操作；
> 本仓库的自动化验证脚本只覆盖 QEMU（无头）。ISO 本身是标准 Hybrid ISO，
> 三种虚拟机加载的是同一个文件。
>
> 若要在图形虚拟机里验证账户持久化与 FAT16，需要再挂两块 IDE 磁盘：
> 主盘（`disk.img`，可写，供 LBA 200-207 存账户表）与从盘（`data.img`，
> FAT16 数据盘）。VirtualBox 在「设置 → 存储 → 添加硬盘」挂 VDI/VMDK，
> VMware 在「虚拟机设置 → 添加 → 硬盘」挂虚拟磁盘；顺序必须是主盘在前。

### 6.4 验收清单（QEMU 已实测通过）

| 操作 | 预期结果 |
| --- | --- |
| `login: zh` / `Password: zh` | 绿色 `Welcome back, zh!` + 绿色 `[zh@zh_OS ~]$` |
| `help` | 彩色 58 条命令列表 |
| `color` | 一屏 16 色板 |
| `ls` / `cd /home/zh` / `pwd` / `cat readme.txt` | ramfs 列目录、切目录、打印路径与文件内容 |
| `mkdir tmp` / `touch a.txt` / `rm a.txt` | ramfs 建目录、建文件、删除，均绿字反馈 |
| 普通用户 `useradd test` | 红字 `Permission denied.` |
| `su root` + `Password: zh` | 红色 `[root@zh_OS ~]#` |
| root `useradd test` | 绿色成功提示（uid=1002, default password: zh） |
| `logout` → `login: test` / `zh` | 绿色 `Welcome back, test!` |
| `passwd` 改密后重新登录 | 新密码生效 |
| 未挂磁盘时 `diskinfo` | 黄字 `no ATA disk detected` + `-hda disk.img` 提示 |
| `-drive ...if=ide` 挂盘后 `diskinfo` | 绿字 `ATA primary master: present` + `persist sectors: LBA 200-207` |
| 挂盘后 `useradd test` → `reboot` → 登录 `test` | 绿字 `Welcome back, test!`（已持久化） |
| 挂盘后 `passwd zh` → `reboot` → `zh`/新密码 | 登录成功，旧密码失败 |
| 未 `fatmount` 时 `fatls` | 红字 `not mounted, run fatmount first` |
| `fatmount` → `fatls /` | 绿字 `fat16 mounted`，列出 `HELLO.TXT ZH.TXT SUB/` |
| `fatcat /hello.txt` / `fatcat /sub/zh.txt` | 打印文件内容（8.3 名大小写不敏感） |
| `ls /`（ramfs）与 `fatls /`（FAT16） | 各自独立，ramfs 仍是 `home/ root/` |
| `shutdown` / `reboot` | 黄字停机 / 重新引导到登录界面 |

## 七、已知限制

- **账户持久化依赖可写磁盘**：账户表默认是内存态静态数组；只有挂了 IDE 可写盘时，
  启动阶段才从 `LBA 200-207` 读回，`useradd` / `passwd` 成功后立即回写。
  只从 ISO/CD 启动时写扇区会失败（返回 `-EROFS`），此时命令仍会成功但追加黄字
  `[warn] disk read-only, changes lost on reboot`，重启后复位为 `zh`。
  密码只存 FNV-1a 64 位哈希（**非密码学哈希**，仅教学演示）。
- **ramfs 全内存态**：目录树与文件内容都在内核堆里，重启清空、不落盘；
  单目录上限 32 个子项、路径最多 8 段、`cat` 单次读上限 4KB（超出截断）。
- **FAT16 只读且功能收窄**：不实现写 / `mkdir` / `rm`；只解析 8.3 短文件名，
  长文件名（LFN）项被跳过；只支持根目录与一层子目录（路径最多 4 段）；
  固定读主通道的从盘，不支持分区表之外的逻辑分区扫描。
- **内存管理**：`mm/heap.c` 是 2MB 静态池的 first-fit 堆（不做块拆分，支持相邻块合并、
  重复 free / 越界 / 超大分配的报错）；其上游 M3 加了 `mm/phys.c` 的 **4KB 页帧位图
  分配器**（解析 Multiboot2 内存图，支持单帧 / 连续 / 带对齐分配，`memmap` 可查）。
  仍然**没有按页映射**（`paging.c` 还是 512 个 2MB 大页恒等映射低 1GB）、**没有缺页
  处理**、**没有独立的用户地址空间**（用户窗口的虚拟地址就是物理地址直通），
  也不做 swap / CoW。
- **无 SMP**：单处理器、无 APIC、无多核启动；启动 banner 只写 `#1`，不再写 `SMP`。
- **定时器有限**：已实现 PIT 100Hz（`drivers/pit.c`，IRQ0 自增 `pit_ticks`），
  用于 `uptime` / `date` 与内核日志 tick；但**没有定时调度、没有 APIC timer、没有墙上时钟
  时间戳**（日志只带 tick 计数，不解析 RTC 之外的绝对时间）；登录连续失败 3 次后
  「按回车继续」仍是用来代替延时的。
- **能 fork/wait、能按路径 exec 换映像，但进程模型仍是最小可用版**：
  M4 给了「每任务内核栈 + 时钟抢占 + 环形轮转 + 用户态异常只杀当前任务」，
  M5 给了「每进程自己的 PML4/PDPT/PD/PT + 独立 CR3 + 4KB 页」，M6 又给了
  「外部用 gcc 编出来的 ELF64 按 PT_LOAD 逐段装进进程、用 e_entry 进 ring3」
  （`taskrun` / `tasks` / `vmspace` / `elf`）。
  进程已经能自己 fork / wait / exec 了（M7 + M8），TASK_MAX=4 就是上限，fork 是**全量复制**
  （没有 COW，父子各占一份物理页）；exec 换下来的旧地址空间会在换 CR3 之后**当场回收**（M12），
  所以连续 exec 不再泄漏页表；argv 已经能跨 exec 传递（M11），但没有 envp；有每进程 fd 表
  （`/bin/*.elf` 与 home 下的文件都能 open/read/close/creat/write/unlink），但没有 dup/pipe/seek、
  没有 O_* 标志、没有 inode 权限位（仍按"root 全树 / 普通用户限 home"放行），`sys_write` 仍只认屏幕；
  每个进程的私有窗口固定 2MB，页表按需建，进程退出后由 `task_reset()` 统一回收。
  没有优先级、睡眠队列、信号、按需分页。Shell 与命令仍运行在 ring 0。
  ramfs 有当前工作目录概念（提示符路径段实时反映 `cd` 结果），但没有真正的
  inode 权限位，只有「root 全树 / 普通用户限自己 home 子树」这一层粗粒度检查。
  外加 Shell 历史的上/下箭头召回；无组合键、无功能键、无 USB 键盘。
- **显示**：仅 VGA 文本模式 80x25 / CP437，无图形模式、无 UTF-8 中文字模。
- **电源管理**：`reboot` 依赖 8042 复位线；`shutdown` 只是 `cli; hlt`，不触发 ACPI 断电。
- **从 ISO 启动没有可写介质**：CD/ISO 无法写入，因此持久化只在挂载可写磁盘时生效。

### ATA PIO 持久化（已实现，B 组第 2 个功能）

- **驱动接口**（`drivers/ata.c`）：`ata_init()` / `ata_read_sector(lba, buf)` /
  `ata_write_sector(lba, buf)` / `ata_disk_present()`，LBA28、主通道 `0x1F0`、PIO 模式、
  每次 512 字节；另外为 FAT16 追加了只读的 `ata_read_sector_slave()`（选从设备 `0xF0`）。
- **错误码**：`ATA_OK 0` / `ATA_ENODEV -1`（无盘）/ `ATA_EIO -2`（读失败）/
  `ATA_EROFS -3`（写失败或介质只读）。全部以返回值上报，**不 panic**。
- **时序**：状态轮询 BSY/DRDY/DRQ，400ns 延迟用连续读四次 `0x3F6` 实现。
- **布局**：账户表固定放在 `LBA 200` 起 8 个扇区（4KB），序列化格式为小端
  `magic "ZHUS"` + `version 1` + `N` + 保留 4 字节，随后 N 条 `name[32] + uid + gid +
  home[64] + shell[32] + pass_hash(64bit) + is_admin + 保留 3 字节`（每条 148 字节）。
- **流程**：`kernel_main` 在 `login_screen()` 之前调 `ata_init()` + `users_load()`，
  并打一行蓝字 `[ATA] user table loaded from disk` 或 `[ATA] no disk, using defaults`；
  `users_load()` 校验 magic/version，失败则静默保留内置账户，无盘时返回 0 走默认。
- **前置条件**：必须挂一块可写磁盘，例如
  `qemu-system-x86_64 -cdrom zh_OS-0.2beta.iso -drive file=disk.img,format=raw,if=ide -m 128`。

### 用户态 M1 / 系统调用 M2 / 物理内存 M3（已实现）

- **M1 ring3**（`kernel/tss.c`、`kernel/ring3.asm`、`kernel/usermode.c`）：
  GDT 重建为 7 项（`0x08/0x10` 内核、`0x18/0x20` 用户 DPL3、`0x28` TSS 占两个槽），
  `ltr` 装载任务寄存器，`TSS.RSP0` 指向 `tss.c` 里专用的 16KB 内核栈
  （不 `ltr` 或 RSP0 填错 → 从 ring3 进中断时三重故障）。
  用户窗口 **M3 起改为运行时向页帧分配器申请 512 个连续、2MB 对齐的物理帧**
  （原先是 `.bss` 里的 2MB 静态数组，现已去掉，内核映像因此少占 2MB）。
  页表仍是 2MB 大页，但**四级都要 U=1**（PML4 / PDPT / PD 大页项），
  只改最后一级是最常见的失败方式；改完必须重载 CR3 刷 TLB。
- **M2 int 0x80**（`kernel/syscall.c`）：
  IDT 0x80 门属性 `0xEE`（P=1 DPL=3 中断门，DPL 不是 3 的话 ring3 触发即 #GP）。
  ABI 贴近 Linux i386 老习惯：`rax` 调用号、`rdi/rsi/rdx` 参数、返回值写回 `rax`。
  已实现 `sys_write(1)` / `sys_ticks(2)` / `sys_exit(3)` / `sys_cpl(4)`；
  `sys_write` 会校验缓冲区是否落在用户窗口内（越界返回 -1）。
  `sys_exit` 走 `usermode_resume`（`ring3.asm`）：把 `rsp` 拨回进 ring3 前保存的
  内核栈指针、恢复 callee-saved 寄存器、`ret` 回调用点 —— 所以 Shell 能原封不动地继续。
- **M3 物理内存**（`mm/phys.c`、`shell/cmd_memmap.c`）：
  解析 Multiboot2 内存图（tag 6；缺失时退回 tag 4 的 mem_lower/mem_upper）。
  位图 1 bit = 1 帧 = 4KB，容量上限 4GB（位图 128KB 在 `.bss`）。初始化顺序：
  先**整张标占用**，再把 `available` 区域放出来，最后收回低 1MB / 内核映像
  （`_kernel_start`~`_kernel_end`）/ 位图自身 / multiboot2 信息结构。
  帧上限只按**最高的 available 段**算——PC 的 E820 常在 4GB 附近报一大段
  `reserved` 空洞，按整张图的最高地址算会得出 "total 4096MB / used 上百万帧" 的假数据。
  接口：`phys_alloc_frames(count, align)` / `phys_alloc_frame()` /
  `phys_free_frames()`，分配出来的帧已清零；释放会拒绝不对齐、越界、重复释放，
  以及试图归还内核映像或低 1MB 的行为。
- **验收脚本**：`bash build/qemu_verify_ring3.sh`（#PF 里 cs=0x001b）、
  `bash build/qemu_verify_syscall.sh`（末行 M2 PASS）、
  `bash build/qemu_verify_memmap.sh`（内存图 + 页帧 + 自测 + syscall 回归）。

### 进程与调度 M4（已实现）

- **为什么不需要 switch_to 那套寄存器保存**：从 ring3 陷入内核时，CPU 已按
  `TSS.RSP0` 换到**该任务自己的内核栈**，`isr_common` 又把 15 个通用寄存器 +
  5 个 CPU 帧字段压成 `struct regs`。也就是说任务的全部现场此刻就躺在它自己的栈上，
  「保存」只是记下这一帧的地址（`cur->frame = r`），「恢复」就是 rsp 指回那帧、
  弹寄存器、跳过 vector/error、`iretq`（`kernel/task_switch.asm` 的 `zh_resume_frame`）。
  Linux 的 `switch_to` 切 callee-saved 是因为它在普通函数调用里切换；这里不是。
- **TCB**（`kernel/task.c`）：`pid / state / kstack_base+top / frame / user_base /
  user_sp / entry / ticks / switches / exit_code / fault_vector / fault_addr`。
  槽位上限 `TASK_MAX=4`；每任务一个 **8KB 内核栈**（页帧分配器给，16 字节对齐）
  与一个 **2MB 用户窗口**（一次要 8MB、按 2MB 对齐：正好凑成 4 个连续大页）。
- **引导帧**：任务第一次运行时没有"被中断的现场"，所以由 `make_boot_frame()`
  照 `struct regs` 的布局在栈顶造一份：15 个寄存器全 0、`rip=entry`、
  `cs=0x1b`、`rflags=0x202`（IF=1，否则永远不会被抢占）、`rsp=用户栈顶`、`ss=0x23`。
- **抢占点**：`sched_tick(r)` 挂在 `isr.c` 的 IRQ0 分支里（`pit_irq_handler()` 之后，
  EOI 已发）。时间片 `TASK_QUANTUM_TICKS=2`，100Hz 下 20ms；用完就
  `cur→READY`、记帧、挑下一个 READY、**重写 `TSS.RSP0`**、`zh_resume_frame()`。
- **三个必须记住的坑**（都写进了 `task.c` 顶部注释）：
  ① 换人前不写 `TSS.RSP0` → 两个任务把中断帧叠在同一块栈上，几十个 tick 后三重故障；
  ② 帧地址必须是"最后一次被换出时的那个 `r`"，不能自己用 `栈顶-sizeof` 算
     （任务可能在 ring0 里被抢占，比如正卡在 `sys_sleep` 的 `hlt` 循环里）；
  ③ `zh_sched_enter` 之前必须 `cli`，否则时钟中断会把正要用的那块栈顶的引导帧踩坏
     （`iretq` 会按帧里的 RFLAGS 把 IF 还回去；回到 Shell 后要显式 `sti`）。
- **异常隔离**：`isr.c` 的 `handle_exception()` 开头新增一个判据 ——
  `cs & 3 == 3` 且调度器在跑，就 `task_fault()` 杀掉该任务并换人，不再 panic 整机。
  内核自己的异常（`cs=0x08`）仍然走原来的 panic dump。
- **两个新系统调用**（M4 顺手补的最低需求）：
  `ZH_SYS_PUTC(5)` 打一个字符（不打 `sys_write` 那行日志，否则屏幕上每秒几十行）；
  `ZH_SYS_SLEEP(6)` = `sti; hlt` 循环等若干个 tick，把 CPU 真正让出去。
- **验收**：`bash build/qemu_verify_task.sh` —— 屏 2 抓运行中（应看到
  `BABABABABABAB` 交错），屏 3 为 `M4 PASS: two tasks ran interleaved`（26 次切换、
  两任务各 exit 0），屏 4 为 `taskrun fault`（B 被 `vector=14 (#PF)`、`cr2=0xb8f34`
  单独杀掉、A 继续打完全部字母），屏 5 复看 `tasks` 表。
  截图转 PNG 用 `python3 build/ppm2png.py build/task_verify/*.ppm`（只用标准库，
  因为 WSL 里没装 PIL）。

### 进程地址空间 M5（已实现）

- **M4 的“隔离”是假的**：所有任务共用内核那一张页表，用户窗口只是不同的 2MB 大页，
  两块大页对 ring3 都是 U=1 —— A 拿到 B 的窗口地址就能读写。真正的隔离必须
  「每个进程一张页表 + 换进程时换 CR3」，这就是 Linux 里 mm_struct / pgd 的核心。
- **地址空间布局**（`kernel/vmm.c`）：内核恒等映射 0~1GB（2MB 大页，U=0，所有进程
  共享同一份 PD）；用户私有窗口放在 **1GB 处** `0x40000000`，2MB 大小（512 个 4KB 页）。
  选 1GB 是因为内核正好占了 0~1GB，后面的 PDPT 槽位本来是空的；又因为 0x40000000
  的 PML4 索引仍是 0，所以每个进程的 PML4 只有 [0] 这一项有用：它指向自己的 PDPT，
  PDPT[0] 抄内核的 PD（共享内核映射）、PDPT[1] 指向自己的 PD。
- **每进程 6 帧**：PML4 / PDPT / PD / PT 各一帧 + 代码页 + 栈页。只有代码页和栈页被
  `vmm_map_4k()` 映射（U=1、可写），其余 510 页保持不存在 —— 这就是 4KB 粒度的意义：
  M4 时 2MB 大页的 U/S 位一置就是整 2MB 全开，做不到“这一页给你、那一页不给”。
- **切换**：`sched_tick` / `schedule_away` / `task_run_all` 在 `zh_resume_frame` 之前
  `mov cr3`（顺带刷掉非全局 TLB 项）。换完还能继续执行，是因为每个进程的地址空间
  都把内核那 1GB 恒等映射（含内核代码、任务的 8KB 内核栈、页帧位图）原样挂了进去；
  所有任务结束后切回 `vmm_kernel_cr3()`。
- **必须记住的坑（M5 第一版就死在这儿）**：新 PDPT[0] 要指向内核的 **PD**，不是内核
  的 PDPT。多抄一层 → 恒等映射整段错位 → `mov cr3` 的下一条指令就 #PF，而且 CR2
  恰好等于 CR3 的值（取指时走的就是那张坏表）。诊断办法：`build/qemu_trace.sh` 用
  `-d int,cpu_reset` 记录全部异常与复位，`build/sym.sh 0x1057e7` 把 RIP 翻成函数名
  加反汇编 —— 这两条比猜快得多。
- **`sys_write` 的指针校验也跟着改**（`syscall.c` 的 `user_ptr_ok`）：M5 之后用户传
  进来的是虚拟地址，窗口基点改为从当前 TCB 取（`task_user_window_current()`），
  内核此刻正跑在该进程的 CR3 上，所以直接解引用就完成了地址翻译。
- **验收**：`bash build/qemu_verify_task.sh`（6 屏）。屏 3 有 `M5 PASS: same VA
  0x40000000 -> different frames, each process has its own CR3`；屏 4 的 `taskrun fault`
  里 B 的 `cr2=0x400ff000`（一个**用户虚拟地址**）、`error code 6`（页不存在 + 写
  + 用户，对比 M4 的 7 = 页存在但权限不够）；屏 6 的 `vmspace` 直接把两张页表打印
  出来：`VA 0x40000000 -> PA 0x357000`（A）与 `-> PA 0x35f000`（B）。
- **调试入口**：`taskrun dry` 只建进程、打印页表、不进 ring3；`vmspace` 随时可看。

### ELF 装载器 M6（已实现）

- **M5 之前的"用户程序"是假的**：`task.c` 里用 emitter 一条条写 `mov eax,1` / `int 0x80`，
  程序是内核自己拼的字节，没有文件格式、没有装载过程。M6 让程序变成**外部产物**：
  用 gcc 编译、ld 链接成标准 ELF64，内核只负责把它搬进进程。这一步的差别不在于指令，
  而在于"内核认识文件格式"——Linux 的 `load_elf_binary()` 干的就是这件事。
- **两段式构建**（`build.sh` 的 `build_user_programs()`）：

  1. `gcc -m64 -ffreestanding -fno-pie -no-pie -nostdlib -O2 -c user/hello.c`，
     再用 `user/user.ld` 链接到 **0x40000000**（M5 的进程私有窗口起点），
     于是程序里所有绝对地址（比如 `.rodata` 里那个字符串的指针）在装进进程后依然有效；
  2. `ld -r -b binary hello.elf -o hello_blob.o` 把整个 ELF 文件当成"只含数据的 .o"
     嵌进内核镜像，生成 `_binary_hello_elf_start/_end/_size` 三个符号。
     这就是 Linux 的 **initrd** 思路：内核里带着文件内容，但还没有文件系统。

- **支持的 ELF 子集**：`ET_EXEC` + `EM_X86_64` + `PT_LOAD`，无重定位、无动态链接、
  无 `PT_INTERP`。`e_phentsize` 必须是 56、`e_phnum` 上限 16、入口必须落在用户窗口内，
  任何一条不满足都在装载前失败并打出一行原因，不会带着坏表进 ring3。
- **装载算法**（`kernel/elf.c` 的 `elf_load_into()`）：对每个 `PT_LOAD`，把
  `[p_vaddr, p_vaddr+p_memsz)` 按 4KB 取整成页，逐页 `vmm_probe()` 查已有映射；
  没有就 `phys_alloc_frame()` 要一帧 + `vmm_map_4k()` 映射（**按段按页分配**，
  而不是 M5 那种"整个窗口先铺满"）；然后把文件里的 `p_filesz` 字节按页切段拷进去，
  `p_memsz > p_filesz` 的尾巴（`.bss`）清零。最后用 `e_entry` 当 `rip`。
- **一个必须注意的细节**：同一页可能被相邻两个段共享，所以"清零"只能在本次装载里
  第一次碰到该页时做（代码里用 512 字节的 `touched[]` 记）；否则会把前一段刚拷进去的
  内容抹掉。反过来，重复 `elf` 时页面是被复用的，必须先清零才不会残留上一次的字节。
- **`elf` 命令的四步**（`shell/cmd_elf.c`）：`elf parse` 只解析（打印 magic/class/type/
  machine/entry/程序头，VGA 只有 25 行，所以单独一条）；`elf` 则解析 → 建进程并装载
  → `task_run_all()` 进 ring3 → 回到内核读 TCB，校验 `state=DEAD`、`exit code=42`、
  `fault vector=0`。用户程序本身用 `int 0x80` 打一行 `hello from a real ELF64 binary (M6)`
  再 `sys_exit(42)`，所以"真的跑起来了"这件事是被退出码证明的。
- **验收**：`bash build/qemu_verify_elf.sh`（3 屏）。屏 2 是纯解析（`phdr 0: type=1 PT_LOAD
  flags=5 (R-X)  vaddr=0x40000000 filesz=0x65 memsz=0x65`）；屏 3 一屏看全流程：
  `pages mapped: 1`、`pid 0  cr3=0x355000`、`sys_write: 36 bytes delivered for ring 3`、
  `state=DEAD exit code=42`、`M6 PASS`。

### 进程的 fork / wait M7（已实现）

- **M6 的进程还不会"生"进程**：`task_create_elf` 是内核从命令里造出来的，
  用户态没有任何办法复制自己。M7 补上 Linux 进程模型里最关键的一块：
  `fork()`（复制当前进程）+ `wait()`（等子进程并收退出码）+ `getpid()`。
- **fork 的核心只有两句话**：把**地址空间**复制一份，把**寄存器现场**复制一份。

  1. 地址空间：`vmm_space_clone()`（`kernel/vmm.c`）新建 PML4/PDPT/PD/PT，
     然后遍历父进程 PT 的 512 项，把每个已映射页 `phys_alloc_frame()` 申请一帧、
     整页拷贝、按原权限映射进子空间 —— **没有 COW**，父子从这一刻起各有一份物理页；
  2. 寄存器现场：把父进程此刻的中断帧（`struct regs *r`，就是 `int 0x80` 压的那 176 字节）
     原样拷到子进程自己的内核栈顶，只把 `rax` 改成 0。

  于是父子都从 `int 0x80` 的下一条指令继续执行，返回值不同：**父 = 子进程 pid，子 = 0**。
  Linux 的 `copy_thread()` 也是这么做的（复制 `pt_regs`、把 `ax` 置 0），
  这才是 fork 返回值语义的真正来源。
- **wait 的阻塞**：父进程调用 `wait()` 时若子进程还活着，就把自己的状态设成
  `TASK_WAIT`（新加的状态，`pick_next()` 会跳过它），`cur->frame = r`，然后
  `schedule_away()` 换人。子进程 `sys_exit` 时 `notify_parent()` 做两件事：
  一是把退出码写进**父进程保存的那个中断帧的 rax**，二是把父进程改回 `TASK_READY`。
  父进程被换回时 `iretq` 用的就是这一帧 —— 从用户态看，`wait()` 这个系统调用"返回"了
  子进程的退出码。这是本内核里第一次出现**真正的阻塞**（M4 的 `sleep` 只是 hlt 空等）。
- **僵尸与收尸**：子进程先退出、父进程后 `wait()` 时，子进程停在 `TASK_DEAD` 并置
  `has_wait_code=1`；父进程 `wait()` 发现这个僵尸，直接取走退出码并把槽位释放回
  `TASK_FREE`。异常被杀的子进程也给父进程留 `-1`。
- **演示程序**（`user/forkdemo.c`，也是 `elf fork` 跑的那个）：父进程 fork 两次，
  子 1 在死循环里把全局 `g_counter` 加到 5 再 `exit(11)`；子 2 立即 `exit(22)`。
  父进程用 `wait()` 收到 11 和 22，并且打印自己那份 `g_counter` 仍然是 0 ——
  一行输出同时证明了"进程复制成功"和"内存没有共享"。
- **一个可读的输出**（`elf fork`，一屏能看全）：

  ```
  [kernel] fork: pid 0 -> pid 1  copied pages=2  child cr3=0x360000
  [kernel] wait: pid 0 blocks until pid 1 exits
    [child 1] g_counter=5 -> exiting with 11
    [parent] wait() -> 11   (child 1 collected)
    [parent] my g_counter is still 0 -> the child's write did NOT leak
  [kernel] fork: pid 0 -> pid 2  copied pages=2  child cr3=0x368000
  [kernel] wait: pid 0 collected exit code 22 from pid 2
    [parent] wait() -> 22   (child 2 collected)
  M7 PASS: fork duplicated a process ...
  ```

- **验收**：`bash build/qemu_verify_fork.sh`（3 屏：提示符 / `elf fork` / `elf` 回归），
  同一张 ISO 上再跑 `bash build/iso_check.sh` 确认 M4/M5 没被破坏。
- **新增系统调用**：`ZH_SYS_FORK 7`、`ZH_SYS_GETPID 8`、`ZH_SYS_WAIT 9`；
  用户程序因此第一次拥有"自己决定跑几个进程"的能力。

### exec 与「按路径装载」M8（已实现）

- **M7 之后进程能生进程了，但还只能"生同一个程序"**：`task_create_elf` 是内核从
  命令里造出来的，用户态没有任何办法把自己换成别的程序，程序也不是文件。
  M8 补上 `exec`：**同一个 pid、同一个进程，映像原地换掉**，而且 ELF 是**按路径从
  ramfs 读出来**的。
- **先让程序变成文件**：`fs/initrd.c` 在启动时把内核里嵌入的三个 ELF 写进 ramfs：

  ```
  [initrd] /bin/hello.elf  4744 bytes (ELF64, ready for exec)
  [initrd] /bin/forkdemo.elf  6688 bytes (ELF64, ready for exec)
  [initrd] /bin/execdemo.elf  5496 bytes (ELF64, ready for exec)
  ```

  这就是 Linux 的 initrd 干的事。`/bin` 按 Linux 惯例做成了**所有人可读**
  （ramfs 没有 mode 位，所以按"根节点下的 bin 子树"来判断），普通用户 `zh` 也能执行。
- **exec 的三个动作**（`syscall.c` 的 `sys_exec` + `task.c` 的 `task_exec_current`）：

  1. **按路径打开**：先把用户态给的路径字符串**逐字节**拷进内核（`user_ptr_ok` 一次校验一字节，
     因为字符串长度未知），再 `ramfs_read_file()` 把内容读进 64KB 的内核缓冲区；
  2. **建新地址空间并装载**：`vmm_space_create()` + 一页新栈 + `elf_load_into()`。
     **不做**"在旧空间上覆盖"——Linux 也是换一套新的 `mm`，旧 `mm` 交给引用计数回收；
     本版没有回收，所以旧页表/页帧就泄漏了（写进已知限制）；
  3. **换映像**：把 TCB 的 `space/cr3/entry/stack_pa/user_sp` 整体换成新的，然后
     `vmm_load_cr3(新 cr3)`，最后改**中断帧**：`rip = e_entry`、`rsp = 新栈顶`、
     `rax` 与其余通用寄存器清零。于是 `syscall_dispatch` 返回后那条 `iretq` 直接落在
     新程序的入口 —— 旧程序从这一刻起就"不存在"了。

  这就是 exec 最反直觉的地方：**它没有返回值**（成功时），因为"调用 exec 的那段代码"
  本身已经被新的地址空间换掉了。Linux 的 `start_thread()` 改 `pt_regs` 是同一招。
- **演示链**（`user/execdemo.c` + `elf exec`）：

  ```
  [zh@zh_OS ~]$ ls /bin
  execdemo.elf 5496   forkdemo.elf 6688   hello.elf 4744
  [zh@zh_OS ~]$ elf exec
  M8 exec demo: load /bin/execdemo.elf from ramfs, it will exec /bin/hello.elf
    opened    : /bin/execdemo.elf  5496 bytes  (read by path, no fd table yet)
    created   : pid 0  entry 0x00000000400000c0  pages 1
  run: the process will print its pid, then replace itself via exec()
  exec demo: pid 0 is about to exec /bin/hello.elf
             (this process image is about to be replaced)
  [kernel] exec: pid 0 image replaced (fresh address space)
  [kernel] exec: entry=0x0000000040000000 cr3=0x0000000000382000
  hello from a real ELF64 binary (M6)
    after run        : pid 0  state=DEAD  exit code=42  execs=1  faults=0
  M8 PASS: exec() opened an ELF by path from ramfs, built a fresh
           space, replaced the running image in place (same pid), and the
           new program exited 42 -- the old program never came back.
  ```

  三件事同时被这一屏证明：①ELF 是**按路径从文件**读的（`opened: /bin/execdemo.elf`）；
  ②**pid 没变**（0 → 0），但程序变了（execdemo 的输出之后是 hello 的输出）；
  ③退出码 **42 是 hello 给的**（execdemo 失败时会退 9）—— 说明真的换成了新映像在跑。
- **验收**：`bash build/qemu_verify_exec.sh`（3 屏：`ls /bin` / `elf exec` / `elf fork` 回归），
  产物在 `build/exec_verify/`。同一张 ISO 上 `bash build/iso_check.sh` 仍应 M4/M5 PASS。
- **新增系统调用**：`ZH_SYS_EXEC 10`，参数是路径指针，失败返回 -1。

### 文件描述符表 M9（已实现）

- **M8 之后还有一个硬伤**：`exec` 能按路径打开 ELF 了，但打开文件的人是**内核**
  （`sys_exec` 里自己调 `ramfs_read_file`）。用户态对文件一无所知，`sys_write` 也只能
  往屏幕上写。M9 把"打开文件"这件事交给进程自己：`open` / `read` / `close` 三个门，
  **每进程一张 fd 表**放在 TCB 里。
- **fd 是什么**：Linux 里 fd 就是一个整数下标，真正的对象藏在进程的 `files_struct`：

  ```
  fd --> struct file --> 偏移 f_pos + 打开方式
  ```

  这里把 `struct file` 简化成 `zfile_t { 用了没, 指向哪个 ramfs 节点, 读偏移 }`，
  `ZH_MAX_FDS=8` 项一张表。所以 `open()` 返回的是**下标**（`fd 0`），与 Linux 一致：
  用户拿不到内核指针，伪造或越界的 fd 一律 `-1`。
- **三个系统调用**（`kernel/syscall.c` + `fs/file.c`）：

  | 调用 | 参数 | 返回 | 内核里做什么 |
  |---|---|---|---|
  | `ZH_SYS_OPEN 11` | `path` | `fd` 或 -1 | 逐字节拷路径 → `ramfs_open_check`（存在/是文件/可读）→ 分配表项 |
  | `ZH_SYS_READ 12` | `fd, buf, len` | 读到的字节数，0 = EOF，-1 = 坏 fd | 按 `off` 读进内核缓冲 → 逐字节过 `user_ptr_ok` 写进用户缓冲 → `off += n` |
  | `ZH_SYS_CLOSE 13` | `fd` | 0 或 -1 | 把表项标成空闲（此后同一个 fd 再 read 就是 -1） |

- **两个必须对上的语义细节**：

  1. **读偏移在 fd 上，不在文件上**（`struct file` 的 `f_pos`）。同一个 fd 连读两次，
     第二次读到的是魔数之后的字节 —— 演示里正是靠这个证明偏移真的前移了；
  2. **fork 继承 fd 表**（`file_table_clone()`）：子进程拿到与父进程相同的 fd 与偏移，
     这正是 Linux 里"fork 之后父子共享已打开文件"的最小版本。
- **演示程序 `user/fddemo.c`（`fd` 命令跑它，从 ramfs 装载）**：

  ```
  fd demo: I am a ring3 program, I will open/read/close myself
    open("/bin/hello.elf") -> fd 0
    read(fd, 4) -> 4 bytes: 7f 45 4c 46
    ^ 7f 45 4c 46 = ELF magic, the fd really reached the file
    read(fd, 4) again -> 4 bytes: 02 01 01 00  (offset moved, not the magic again)
    close(fd) -> 0
    read(fd) after close -> -1 (fd is dead, as expected)
    open("/bin/nope.elf") -> -1 (no such file)
    open("/bin") -> -1 (it is a directory, not a file)
  fd demo: all five checks passed
    fd table of the process (ZH_MAX_FDS=8):
      open ok=1  open denied=2  read=2  close=1
    after run        : state=DEAD  exit code=0  faults=0
  M9 PASS
  ```

  这一屏里**没有一个字节是内核替它读的**：魔数是从 fd 读出来的，偏移是 fd 自己
  前移的，close 之后失效也是内核按 fd 表拒绝的。程序自检五项全过才 `exit(0)`，
  内核再按计数器（open ok=1 / denied=2 / read=2 / close=1）二次核对 —— 用户说"我读到了"
  和内核说"你确实读过"必须一致。
- **验收**：`bash build/qemu_verify_fd.sh`（3 屏：`ls /bin` / `fd` / `elf exec` 回归），
  产物在 `build/fd_verify/`。
- **新增系统调用**：`OPEN 11`、`READ 12`、`CLOSE 13`；命令表因此变成 **37 条**。

### 可写文件与 fork→exec→wait 闭环 M10（已实现）

- M9 之后文件只能读、进程还不会"执行一条外部命令"。M10 补两件事，正好是 Linux
  里"shell 能跑命令"所需的全部内核能力：**文件可写**，以及 **fork + exec + wait 串成链**。

**一半：文件可写（`creat` / `write` / `unlink`）**

- 新增系统调用 `CREAT 14`、`FWRITE 15`、`UNLINK 16`。`creat` 与 Linux 的
  `creat(path, 0666)` 对应：父目录可写才允许，目标不存在就建、存在就清空（O_TRUNC）。
- `write` 落进 ramfs 的方式是**追加**（`ramfs_node_append`）：重新 `kmalloc` 一块更大的、
  拷旧内容、接上新内容、`kfree` 旧的。所以文件内容真的变了，别的进程再 `open` 能读到 ——
  这是"页缓存增长"的最小版本，而不是往某个临时缓冲区里一塞。
- 演示 `wr`：ring3 程序 `creat("/home/zh/m10.txt")` → 向同一个 fd 追加两段 → `close`
  → 再**只读** `open` 读回 68 字节并打印出来。紧接着在同一次开机里 `cat /home/zh/m10.txt`
  也能看到同样两行 —— 说明内容留在文件系统里，不是程序自己的内存。

**另一半：fork → exec → wait（`spawn`）**

- 这三步串起来就是**一条外部命令**的全部内核动作，Linux 的 shell 每跑一条命令都这么做：

  ```
  pid = fork();
  if (pid == 0) exec("/bin/xxx.elf");   // 子进程：换成新程序，自己的 mm 也换掉
  else          wait(&code);            // 父进程：进 TASK_WAIT，等子进程退出码
  ```

- 演示 `spawn` 的一屏（注意两处 cr3 不同：fork 出来的子进程有自己的地址空间，
  exec 又给它换了一套新的）：

  ```
  [zh@zh_OS ~]$ spawn
  spawn demo: pid 0 will fork, the child will exec, the parent will wait
  [kernel] fork: pid 0 -> pid 1  copied pages=2  child cr3=0x00000000003ad000
  [parent] fork returned child pid 1
  [kernel] wait: pid 0 blocks until pid 1 exits
  [child ] fork returned 0, I am pid 1, now replacing myself with hello.elf
  [kernel] exec: pid 1 image replaced (fresh address space)
  [kernel] exec: entry=0x0000000040000000 cr3=0x00000000003b3000
  hello from a real ELF64 binary (M6)
  [parent] wait() -> 42   (child 1 collected)
  spawn demo: fork -> exec -> wait closed the loop in one process tree
    after run        : state=DEAD  exit code=0  faults=0
    kernel counters  : forks +1  execs +1  waits +1
  M10 PASS (processes): ...
  ```

  父进程从头到尾只做三件事：fork 出子进程、阻塞等它、拿到 42。中间的 fork 返回值语义
  （父 >0 / 子 0）、exec 换映像、wait 被唤醒写回 rax，全部沿用 M7/M8 的机制 ——
  这一步没有发明新东西，只是把它们**接起来**，而接起来之后行为就与 Linux 一致了。
- **验收**：`bash build/qemu_verify_m10.sh`（4 屏：`wr` / `cat` 读回 / `spawn` / `fd` 回归），
  产物在 `build/m10_verify/`。
- **新增系统调用**：`CREAT 14`、`FWRITE 15`、`UNLINK 16`；命令表 **37 → 39 条**。

### exec 携带 argc/argv M11（已实现）

- **问题**：M8 的 `exec(path)` 只能带一个路径。Linux 的 `execve(path, argv, envp)`
  却要把参数一起交给新程序 —— 没有 argv，"执行一条命令"就没有命令名、没有 `-l` 这种开关，
  shell 也永远没法降到 ring 3。

- **难点（也是这一步的核心）**：参数字符串**不能留在旧地址空间**。
  `exec` 的第一件事就是换掉整个 mm，旧页表连同里面的字符串一起作废；所以字符串必须
  写进**新**地址空间。做法：
  1. 新地址空间建好、新栈页分好（`stack_pa`，页帧恒等映射），直接按物理地址往页里写；
  2. 字符串池从栈页偏移 0 起，每个参数最长 96 字节、最多 16 个；
  3. 指针数组接在字符串后面、16 字节对齐，末尾写一个 `NULL`（与 C 标准一致）；
  4. 写指针时用的是**虚拟地址**（`stack_va + offset`）——内核用 PA 写、程序用 VA 读；
  5. 最后 `r->rdi = argc`、`r->rsi = &argv[0]`，`rip`/`rsp` 换到新映像。
  于是新程序的 `void _start(unsigned long argc, char **argv)` 直接按 C ABI 就拿到了参数；
  老程序（`void _start(void)`）不理会 rdi/rsi 也照跑，`argv` 为 NULL 时 argc=0。

- **两个真踩进去的坑（都很 Linux）**：

  | 现象 | 根因 | 修法 |
  |---|---|---|
  | 子进程 ring3 被杀，`vector=6`(#UD)，`execs +0` | 内核从没置 `CR4.OSFXSR/OSXMMEXCPT`，gcc 自动向量化出的 `movq (%rip),%xmm0` 在 ring3 直接 #UD | `paging.c` 的 `enable_pae()` 里加 `0x200|0x400`，并新增 `enable_fpu()` 清 `CR0.EM/TS` |
  | 同一个地址改成 `vector=13`(#GP)，停在 `movaps %xmm0,(%rsp)` | gcc 假定函数是被 `call` 进来的（入口 `rsp ≡ 8 (mod 16)`），而 `iretq` 给的是 16 对齐的栈 | 用户栈顶从 `SPAN-16` 改成 `SPAN-8`，让入口遵守 SysV 的对齐约定 |

  这两个坑都不是"程序写错了"，而是**内核少做了硬件/ABI 义务**：不开 SSE，今后任何被
  编译器向量化的用户程序都会莫名 #UD；不对齐栈，任何用到 `movaps` 的函数都会 #GP。

- **演示 `argv`**：`user/argvdemo.c` 一个程序扮演两个角色（用 `argc` 区分，避免无限 fork）：
  `argc==0` 时（内核装载器起的）打印"我没有 argv"，然后 `fork`，子进程
  `exec("/bin/argvdemo.elf", {"argvdemo","alpha","beta"}, 3)`，父进程 `wait`；
  `argc>0` 时（被 exec 起的）打印 `argc` 与每个 `argv[i]` 后 `exit(0)`。

  ```
  [zh@zh_OS ~]$ argv
  argv demo: pid 0 starts with argc=0 (loaded by the kernel, no argv)
  [kernel] fork: pid 0 -> pid 1  copied pages=2  child cr3=0x00000000003bf000
  [kernel] exec: pid 1 image replaced (fresh address space)
  [kernel] exec: entry=0x00000000400000d0 cr3=0x00000000003c5000
  [child ] I was exec()d with argc=3  (pid 1)
            argv[0] = "argvdemo"
            argv[1] = "alpha"
            argv[2] = "beta"
  [parent] wait() -> 0
  M11 PASS: exec() carried argc=3 and argv[] into the new image.
  ```

  一屏里同时有三件事：两处 cr3 不同（fork 一次、exec 再一次换 mm）、argc 从 0 变 3、
  三个字符串**跨过了 exec 边界**。

- **接口**：`sys_exec` 现在是 `(r, path_ptr, argv_ptr, argc)`，分发处传 `rdi/rsi/rdx`；
  内核先读用户态的 8 字节指针数组（每个都过 `user_ptr_ok`），再逐字节拷每个字符串到
  `g_exec_args[8][97]`。老程序传 0 天然等价于无参 exec。
  `task_exec_current()` 保留原签名（转调 `task_exec_current_argv()`），所以 M8 的调用点全不用改。
- **验收**：`bash build/qemu_verify_argv.sh`（2 屏：`argv` + `spawn` 回归），产物在
  `build/argv_verify/`；全量回归 `bash build/regress_all.sh`（M2/M4/M5/M6/M7/M8/M9/M10/M11 一次跑完）。
- **命令表 39 → 40 条**。


### 地址空间回收 M12（已实现）

- **问题**：M11 之后 `exec` 每跑一次就换一整套 mm，旧的那套**从来没还过**。
  一次 exec 泄漏 4 张页表 + 1 栈页 + N 代码页 ≈ 6 帧；连着跑二三十次命令，
  8MB 的帧池就见底了（真实症状是"用久了以后 elf 说 no frames left"）。

- **做法**：`vmm_space_destroy()`（Linux `mmput()` → `free_pgtables()` 的雏形）：
  1. 扫私有窗口那**一个** PT 的 512 项，每项 PRESENT 就把背后那一帧还给分配器
     （代码页、栈页、fork 复制出的页都在里面）；
  2. 再把 PT / PD / PDPT / PML4 这四帧自己也还回去 —— 这四帧最容易漏；
  3. 内核共享的那个 PD 不能动（它属于所有进程），函数里显式挡住"销毁内核空间"。

- **顺序是硬性前提**：必须**先 `mov cr3` 换到新空间，再销毁旧的**。
  反过来的话，刚 `phys_free_frame` 掉的页表下一秒就被 CPU 当成当前页表用，
  现象是下一行指令 #PF，而且 CR2 恰好等于你刚释放的地址。代码里这一步和
  `vmm_load_cr3(t->cr3)` 紧挨着，注释也写死了。

- **两个回收点**：

  | 时机 | 谁在回收 | 为什么安全 |
  |---|---|---|
  | `exec` 换完 CR3 之后 | `reclaim_space(&old_space)` | CR3 已经站在新空间上，旧空间没人用 |
  | 命令之间 `task_reset()` | 遍历所有非 FREE 任务的 `space` | 此时 CR3 一定是内核自己的（`task_run_all` 返回时已换回） |

- **怎么看见它**（`mm` 命令）：跑 `/bin/reexec.elf` —— 那个程序靠 argv 把"剩余深度"
  传下去，连续 exec 自己 7 次。每次 exec 都建一整套新 mm，如果回收缺失，
  7 次就得吃掉 40 多帧。

  ```
  [zh@zh_OS ~]$ mm
    frames    : total=32736  used=3015  free=29721
  [reexec] pid 0: depth 6  -> exec myself with 5
  ...（7 行，每次 exec 一行）
  [reexec] pid 0: depth 0 reached, no more exec. exiting 0
    after run : total=32736  used=3023  free=29713
    execs     : +6   reclaimed: +6 mm / +12 pages
    free      : before=29721  after=29713  delta=8
  M12 PASS: every exec() destroyed the mm it replaced.
  ```

  `delta=8` 就是"最后一个还活着的进程那一套 mm"，**与 exec 次数无关**：
  `+6 mm` 说明 6 次 exec 都把旧的还了。再跑一次 `mm`，delta 变成 6、
  终点仍是 29713 —— 账是稳的，不累积。

- **踩到的一个假警报（值得记下来）**：第一次跑 `mm` 报 "leaked 2056 frames"。
  根因不是漏洞，而是**测量点选错了**：`task_init()` 第一次被调用时会一次性分配
  `TASK_MAX × 2MB = 8MB` 的用户窗口池（2048 帧），而第一次跑 `mm` 恰好替别的命令
  把它触发出来了，于是这 2056 帧混进了差值。修法是命令里先 `task_init()` 再开始量账 ——
  **先量固定成本，再量要观察的行为**，否则测出来的是"第一次启动的成本"而不是"泄漏"。
- **验收**：`bash build/qemu_verify_m12.sh`（3 屏：`mm` / 再跑一次 `mm` / `elf exec` 回归），
  产物在 `build/m12_verify/`；全量回归 `bash build/regress_all.sh`（M2…M12 一次跑完）。
- **命令表 40 → 41 条**。

### Shell 降到 ring 3 M13（已实现）

- **问题**：M12 之前 shell 是内核里的一个循环 + 一张函数表 —— 它自己在 ring 0，
  命令是内核函数。所谓"用户敲一条命令"，其实没有任何进程被创建、没有任何特权切换。
  这是整条路线里最后一块"观感差距"：**特权边界没有压在命令与内核之间**。

- **先补三条系统调用**（`kernel/syscall.h`，17/18/19）：

  | 号 | 名字 | 做什么 | 为什么非它不可 |
  |---|---|---|---|
  | `17` | `SYS_READKEY` | 从键盘环形缓冲取**一个字符**，空了返回 0 | 以前整行是内核 `read_line()` 读的，用户态碰不到键盘 |
  | `18` | `SYS_CHDIR` | 转发 `ramfs_chdir()` | `cd` 得是真的改目录，不能只改提示符 |
  | `19` | `SYS_GETCWD` | 把当前目录拷进用户缓冲 | 提示符里的路径得由 shell 自己要来 |

  `SYS_READKEY` 故意做成**非阻塞**：缓冲区空就返回 0，由用户态自己 `SYS_SLEEP(1)`
  再问一次。为什么不阻塞？因为内核里阻塞要挂起任务 + 等待队列 + 有人唤醒，
  那是信号量的活；这里先让 shell 自己轮询，语义照样正确，只是更笨。

- **`/bin/zhsh3.elf`（`user/zhsh3.c`，9064 字节）**：一个真的 ring 3 shell。
  它能碰的东西只有系统调用，一次外部命令就是三段：

  ```
  pid = fork();
  if (pid == 0) exec(path, argv);   // 子进程换成 /bin/<名字>.elf
  code = wait();                    // 父进程等它退出、拿退出码
  ```

  这几行和 Linux 的 `sh` 做的是同一件事 —— 因为没有别的办法：ring 3 里它既不能
  直接调用内核的 `cmd_ls()`，也碰不到 `ramfs` 结构体，只能 fork+exec。

- **内置 vs 外部**（和 Linux 同样的分法）：`help` / `exit` / `echo` / `pwd` / `cd`
  由 zhsh3 自己实现（builtin，改了目录、改了它自己的状态）；其余全是外部程序，
  名字按 `PATH` 规则找 —— 本版的"PATH 搜索"就是一行代码：不带 `/` 的名字拼成
  `/bin/<名字>.elf`。所以敲 `hello` 会跑 `/bin/hello.elf`，敲 `pwd` 这种拼错的
  名字会看到 `cannot exec (no such program in /bin)` + 退出码 127 —— 和 shell 应有的行为一致。

- **一屏里能看到什么**（`sh3` 命令，4 屏验屏）：

  ```
  [zh@zh_OS ~]$ sh3
  M13 ring-3 shell: /bin/zhsh3.elf will run in user mode (CPL=3)
  zhsh3: I am a real ring-3 program (pid 0), reading the keyboard one key at a time
  zhsh3: every external command is fork() -> exec() -> wait(), like Linux

  zh3:~$ help                      <- builtin，由 shell 进程自己打印
  ...
  zh3:~$ echo hello from ring3     <- builtin
  hello from ring3
  zh3:~$ cd /bin                   <- builtin（SYS_CHDIR）
  zh3:/bin$ pwd                    <- builtin（SYS_GETCWD），提示符也跟着变
  /bin
  zh3:/bin$ hello                  <- 外部程序：fork -> exec -> wait
  hello from a real ELF64 binary (M6)
  
  zh3:~$ exit
  zhsh3: exit (code 0)
  M13 PASS: the shell itself ran in ring 3 and its commands were real processes.
  ```

- **内核这一侧的活只剩一行**：`task_create_elf("zhsh3", blob, len, &img)` + `task_run_all()`。
  之后屏幕上发生的一切（提示符、读键、回显、拆参数、fork/exec/wait）都是 ring 3 里那个
  进程自己做的 —— 内核不再替它读整行，也不再替它执行命令。
- **保留了内核 shell**：它是 fallback，`zhsh3.elf` 起不来时不至于变砖（和 Linux 的
  `init=/bin/sh` 思路一样）。所以现在系统里有**两层 shell**：ring 0 的调试 shell，
  和 ring 3 的真 shell。
- **踩到的两个小坑**：
  1. `sys_write` 的逐条日志会把屏幕刷爆（ring3 shell 每条命令都在写），
     `cmd_sh3` 里 `syscall_set_write_log(0)` 关掉；
  2. `exit` 一开始沿用上一条命令的退出码（敲了 `argv` 失败后 `exit` 变成 127），
     改成 `exit [n]`：无参退 0，`exit 3` 才退 3 —— 与 Linux `exit` 语义一致。
- **验收**：`bash build/qemu_verify_m13.sh`（4 屏：启动+help / 内置命令 / 外部命令 / exit+PASS），
  产物在 `build/m13_verify/`；全量回归 `bash build/regress_all.sh`（M2…M13）。
- **命令表 41 → 42 条**。

### ls / cat 也变成用户态程序 M14（已实现）

- **动机**：M13 之后还有一条"观感差距" —— 敲 `ls` 时 shell 会去找 `/bin/ls.elf`，
  然后失败。因为 `ls`/`cat` 一直是内核里的 `cmd_ls()` / `cmd_cat()`：它们直接摸
  `ramfs_node_t` 的 `children` 链表。**缺的不是程序，是一个系统调用。**

- **新增 `SYS_READDIR 20`**（Linux `getdents` 的雏形）。本 ABI 只有 rdi/rsi/rdx 三个参数位，
  所以签名定成：

  ```
  rax = 20;  rdi = 目录路径（NULL/空 = 当前目录）;  rsi = 序号;  rdx = 名字缓冲
  返回：0 = 文件，1 = 目录，-1 = 枚举结束（或不是目录、越权）
  ```

  名字缓冲长度**固定 64**（因为只有三个参数位，没有第四个位置放 cap；也正好
  等于 `ramfs_node_t.name` 的容量）。内核侧 `sys_readdir()` 先 `copy_path_from_user`，
  再调 `fs/ramfs.c` 新增的 `ramfs_readdir(path, index, name, cap, &type)` ——
  它按序号在 `dir->children` 上走 n 步，带回名字与类型，走完返回 `RAMFS_ERR_NOENT`。

  为什么用"序号"而不是"目录句柄"？本版没有 fd 之外的进程资源概念，序号已经够让
  `ls` 一页页翻完一个目录，而且调用方（用户态）不需要持有、也不需要 `close` 任何东西。

- **`/bin/ls.elf`（`user/ls.c`，5544 字节）**：`ls` 与 `ls <dir>`。无参时先
  `SYS_GETCWD` 拿当前目录，再 `for (index = 0; ; index++)` 一遍遍 `SYS_READDIR`，
  目录后面补 `/`，最后打印

  ```
  m10.txt
  (1 entries, read one by one through SYS_READDIR)
  ```

  那行 `(N entries, read one by one through SYS_READDIR)` 是刻意留的证据：它说明这
  N 个名字**没有一个是内核一次塞给它的**，全是它一个一个 syscall 问出来的。
  非目录/空目录打印 `ls: <path>: empty or not a directory` 并 `exit 1`。

- **`/bin/cat.elf`（`user/cat.c`，6248 字节）**：走 Linux 的老三样 —— `open` / `read` /
  `close`（M9 的 11/12/13），而不是让内核一次 `ramfs_read_file()` 把整份内容塞过来：

  ```
  fd = open(path);
  while ((n = read(fd, buf, 256)) > 0) write(1, buf, n);   // 一次 256 字节
  close(fd);
  ```

  它是 M9 fd 表的第一次"真实使用"：一个用户态程序按偏移一段一段读文件，意味着
  fd 里那个 `off` 字段真的在起作用（`read` 只认 fd，不再回头查路径）。

- **一屏里能看到什么**（`build/qemu_verify_m14.sh`，3 屏）：

  ```
  [zh@zh_OS ~]$ wr                 <- 先用内核的 wr 造出 /home/zh/m10.txt（M10）
  hello from a ring3 file write
  this line was appended by the same fd
  [zh@zh_OS ~]$ sh3
  zh3:~$ ls
  m10.txt
  (1 entries, read one by one through SYS_READDIR)
  zh3:~$ ls /bin
  cat.elf  ls.elf  zhsh3.elf  reexec.elf  argvdemo.elf  ...  hello.elf
  (11 entries, read one by one through SYS_READDIR)
  zh3:~$ cat /home/zh/m10.txt
  cat: open("/home/zh/m10.txt") -> fd 0  (ring 3, one 256-byte chunk at a time)
  -----------------------------------------------------------
  hello from a ring3 file write
  this line was appended by the same fd
  -----------------------------------------------------------
  cat: EOF after 68 bytes, close(fd) -> 0
  ```

- **`ls /bin` 里那 11 个 elf 就是全部用户态程序**：hello / forkdemo / execdemo / fddemo
  / wrdemo / spawndemo / argvdemo / reexec / zhsh3 / ls / cat。它们是 initrd 嵌进内核
  镜像再 install 到 ramfs 的（`fs/initrd.c`），所以 `ls` 列的是**真的文件系统内容**，
  不是一张硬编码字符串表。

- **顺带把 ring3 shell 里的内核日志关掉了**：M12 的 exec 日志、fork/wait 日志原本逐条刷屏，
  `cmd_sh3` 里 `task_set_exec_verbose(0)` + `syscall_set_write_log(0)` 一起关；
  这两个开关现在同时管 exec / fork / wait 三处（`kernel/task.c` 里把 fork 与 wait 的
  `[kernel]` 行也放到同一个 `g_exec_verbose` 后面）。屏幕才归 shell 自己。

- **里程碑意义**：到这里"**所有命令都是独立进程**"不再有例外 —— 内建只有 shell 的
  `help/exit/echo/pwd/cd`（它们必须改 shell 自己的状态），其余每一条都是
  `fork → exec(/bin/<名字>.elf) → wait` 出来的真进程，`ls` 和 `cat` 也一样。
  和 Linux 的差别只剩"这些工具我是自己写的、只有几十行"而已。

- **验收**：`bash build/qemu_verify_m14.sh`（3 屏），产物 `build/m14_verify/`；
  全量回归 `bash build/regress_all.sh`（M2…M14）。命令表仍是 **42 条**（`ls`/`cat`
  本来就是内核命令，这次是把它们的**用户态替身**加进 `/bin`）。
  ISO 重新生成后 5,748,736 字节（M13 版 5,734,400，+14,336 = ls.elf 5544 + cat.elf 6248 + 少量表格）。

### 写时复制 fork（COW）M15（已实现）

- **问题**：M5/M7 的 fork 是"每个已映射页 `phys_alloc_frame()` + 整页 memcpy"。
  父子进程如果只是 fork 完立刻 exec（shell 敲一条命令的典型情形），那些复制出来
  的页**一秒都不会被读**，纯浪费。Linux 的做法不是"少复制一点"，而是**根本不复制**。

- **机制（三层）**：

  1. **页帧引用计数**（`mm/phys.c`，新增）：一张 `g_frame_ref[帧号]` 表，记录"几个页表项
     指着这一帧"。`phys_share_frame(pa)` 加一，`phys_release_frame(pa)` 减一并返回
     "是否真的归还了"。没有它，父进程退出时会连子进程还在用的帧一起还给分配器 ——
     那是典型的 use-after-free。
  2. **只读 + COW 标记的映射**（`kernel/vmm.c`）：`vmm_map_4k_cow()` 把页映射成
     `PRESENT|USER|COW`，**不带 `WRITABLE`**。COW 用的是 PTE 第 9 位（x86-64 留给 OS
     的三位之一，Linux 用的是同一类位 `_PAGE_BIT_SOFTW1`）。
  3. **缺页时复制**（`vmm_cow_fault()`，Linux `do_wp_page()` 的雏形）：收到 #PF 且满足
     "页存在 + 是写 + 那一项带 COW" → 申请一帧新的、拷 4KB、把 PT 项改成可写并清 COW、
     老帧 `phys_release_frame()` 减一。修好后 `iretq` 回去**重试那条写指令**。

- **fork 现在做什么**（`vmm_space_clone`）：

  ```
  for (每一页 p：父进程有映射) {
      vmm_map_4k_cow(child, va, p);              // 子进程指向同一帧，只读
      父进程 PT 项 = 同一帧 | PRESENT|USER|COW;  // 去掉 W，父进程也变只读
      phys_share_frame(p);                       // 引用计数 +1
  }
  ```

  所以命令行里那句 `copied pages=0` 是**真的 0**（M7 时这里是 1~3）。共享了几页由
  `shared=N` 报出来。fork 的成本从"与常驻内存成正比"降到"4 张页表帧"。

- **为什么父进程那一项也要改成只读**：这是最容易漏的一步。只把子进程设成只读，
  父进程一写就直接改到共享帧里，子进程立刻看到脏数据 —— COW 就成了假的。
  必须两边都只读，谁写谁先分裂。

- **踩到的坑（这一版最值得记的一个）**：第一版 `vmm_cow_fault()` 里写着
  "要求出错码 bit2（U/S）= 1，只修来自用户态的缺页"，结果跑 `sh3` 时内核直接
  `KERNEL PANIC: Page Fault`，`error code=0x3`、`cr2=0x40001080`、`cs=0x08`（ring 0）。
  原因是**本版内核没有 `copy_to_user()`**：`sys_getcwd` / `sys_readdir` 是内核
  **直接往用户缓冲区写**的，而 fork 之后父进程那页已经是只读 + COW。
  修法：判据里删掉特权级这一条 —— 只要是"present + write + 本窗口的 COW 页"，
  不管谁写都得先拿一份自己的副本。**也别图省事把 CR0.WP 置 0 绕过去**：
  那样内核的写会直接落进共享帧，把子进程那一份污染掉，COW 语义就毁了。

- **一屏里能看到什么**（`cow` 命令，`build/qemu_verify_m15.sh` 第 1 屏）：

  ```
  [zh@zh_OS ~]$ cow
  M15 copy-on-write: fork() shares pages, only writes pay for a copy
    program   : /bin/cowdemo.elf
    before    : used=4081  free=28655
  run: the child writes its copy of a shared page, then the parent writes its own
  [cowdemo] g_value starts at 0x00001111   (the page is shared with the parent)
  [parent] after my write: g_value=0x00005555  writes=0x00000001
  [child ] after my write: g_value=0x0000aaaa  writes=0x00000001
  [parent] child exited with 0x00000000
  [parent] after the child is gone, my page still reads 0x00005555
  [cowdemo] child=0xaaaa  parent=0x5555  original=0x1111  -> isolated
    after     : used=4097  free=28639
    fork      : shared=2 page(s) mapped read-only+COW  (0 frames copied at fork)
    on write  : copied=4  page fault(s)=4  (only the pages each side wrote)
  M15 PASS: fork mapped the parent pages into the child as read-only+COW.
  ```

  读法：**fork 时 0 帧被复制**，只共享了 2 页（代码页 + 栈页）；真正付钱的是写过的页 ——
  父子各自写了自己的数据页和栈页，所以共 4 次 #PF、4 次复制。如果父子什么都不写，
  这 4 次也不会发生（Linux 在 fork 之后立刻 exec 的场景里就是这么省的）。

- **M12 的回收也跟着改**：`vmm_space_destroy()` 里用户页从 `phys_free_frame()` 换成
  `phys_release_frame()` —— 共享帧只减计数不归还，减到 0 才真的回分配器。
  这样"父进程先退出、子进程还活着"也不会踩到已被回收的帧。

- **验收**：`bash build/qemu_verify_m15.sh`（3 屏：`cow` / `mm` / `sh3`+`ls /bin`），
  产物 `build/m15_verify/`。第 2、3 屏是**回归证据**：COW 改的是缺页路径，
  最容易连带弄坏的就是 exec 的地址空间回收（M12）和 ring3 shell（M13）——
  两屏都 PASS，说明新路径和旧机制共存。命令表 42 → **43 条**（新增 `cow`）。
  ISO 重新生成后 5,758,976 字节（M14 版 5,748,736，+10,240 = cowdemo.elf 6392 + 命令表与代码）。

### 按需分页 M16（已实现）

- **问题**：到 M15 为止，装载一个程序时 `.bss` 是"整段真的分配 + 整页清零"的。
  程序声明 `char buf[64*1024]` 却只用 3 个字节，内核照样给它 16 帧 —— 一分不少。
  Linux 不这么干：页表项先**不置 present**，谁碰谁触发缺页，那一刻才给页。

- **两种"页还不存在"的原因，本版把它们分清楚了**：

  | 情形 | 出错码 P 位 | 怎么办 |
  |---|---|---|
  | 从没映射过，但装载器登记过"这段是零" | `P=0` | 分配一帧（分配器返回的帧本身已清零）→ 映射（M16） |
  | 映射过、但只是共享（父子 COW），且这次是**写** | `P=1` + `W=1` + PTE 第 9 位 | 复制一帧 → 改成可写（M15） |
  | 其他（野指针、栈溢出、越权） | 任意 | 交给上层：ring3 杀进程 / ring0 panic |

- **`vmm_space_t` 里多了一张小表**：`uint8_t desire[512]`（512 字节/进程），
  每个窗口页一个字节：0 = 不关心；`VMM_WANT_WRITE` = 这一页将来应该存在且可写。
  它相当于 Linux 的 `vm_area_struct`（"这段虚拟地址属于谁、什么权限"）的**极简版**：
  本版的用户窗口只有 2MB，所以用一张定长表就够了，不需要区间树。

- **装载器的改动只有一处**（`kernel/elf.c`）：对"本页 ∩ 文件内容为空"的纯 `.bss` 页，
  不再 `phys_alloc_frame()` + 清零，改成 `vmm_space_want(va, 4096, VMM_WANT_WRITE)`
  然后 `continue`。半个页有文件内容的那种"混合页"仍然照旧拷内容（`.bss` 尾部清零），
  因为那页必须真的在。

- **统一缺页入口**（`kernel/vmm.c` 的 `vmm_page_fault()`，Linux `handle_pte_fault` 的位置）：

  ```
  vmm_page_fault(va, err)
    ├─ vmm_cow_fault(va, err)          // M15：present + write + COW  -> 复制一帧
    └─ vmm_demand_fault_here(&s, va, err)
         // M16：!present + desire!=0 -> phys_alloc_frame()（已清零）-> 映射
         // 权限位取自 desire：VMM_WANT_WRITE -> |PAGE_WRITABLE
  ```

  `isr.c` 那边一行没改逻辑，只是把调用从 `vmm_cow_fault` 换成了 `vmm_page_fault` ——
  这就是把"缺页"从"一个里程碑的专用补丁"变成"一条正常的处理流水线"。

- **fork 会继承"愿望"**：`vmm_space_clone()` 现在把 `desire[]` 整张复制给子进程，
  否则父进程登记过、还没碰的页，到了子进程里一碰就变成真异常。
  `vmm_map_4k` / `vmm_map_4k_cow` 在映射时会把对应那一格清 0（承诺已兑现）。

- **一屏里能看到什么**（`demand` 命令，`build/qemu_verify_m16.sh` 第 1 屏）：

  ```
  [zh@zh_OS ~]$ demand
  M16 demand paging: a page costs nothing until it is touched
    program   : /bin/demanddemo.elf
    frames    : free=28637
    image     : segments=1  mapped pages=1  registered-on-demand=16
  [demand] 64 KB .bss = 16 pages registered, 0 frames paid at load time.
  [demand] read  page 5  -> 0   (first touch: #PF -> a fresh zero page appeared)
  [demand] read  page 10 -> 0   (4 pages skipped: they are still holes, not RAM)
  [demand] read  page 15 -> 0   (another 4 pages skipped)
  [demand] wrote page 5=171  page 10=17  page 15=205
  [demand] 3 of 16 pages materialised; the other 13 never took a frame.
    cost@load : 8 frames (page tables + the file-backed pages only)
    cost@run  : 3 frames (only the pages the program actually touched)
    demand    : faults=3  pages materialised=3  (the other pages were never in RAM)
  M16 PASS
  ```

  `mapped pages=1` 与 `registered-on-demand=16` 是两个**分开的账本**：前者是"真的映射了"，
  后者是"答应了但还没给"。`cost@load` 8 帧是页表 4 帧 + 代码/数据页 + 栈页这类固定开销，
  `cost@run` 3 帧才是程序自己碰出来的。

- **一个必须讲清楚的点（也踩过）**：**读**一页同样会触发 #PF —— 按需分配不区分读写。
  所以"遍历 16 页验证它们都是 0"这种做法本身就把 16 页全买回来了。第一版 demo 就是这么
  写的（跑出 `faults=10`），改成只碰 3 页（且跳着碰：5 / 10 / 15），数字才对上。
  这也解释了 Linux 里 `memset(buf, 0, 64K)` 之后 RSS 会立刻涨 16 页的原因。

- **验收**：`bash build/qemu_verify_m16.sh`（3 屏：`demand` / `cow` / `sh3`+`ls /bin`），
  产物 `build/m16_verify/`。第 2、3 屏同样是回归证据：缺页入口被改写，最容易弄坏的是
  COW（M15）和 ring3 shell（M13）——两屏都 PASS。命令表 43 → **44 条**（新增 `demand`）。
  ISO 重新生成后 5,769,216 字节（M15 版 5,758,976，+10,240 = demanddemo.elf 6472 + 命令表与代码）。

### 信号 + waitpid + Ctrl-C M17（已实现）

- **为什么信号不能“立刻”送**：一个进程可能正在 ring3 里自旋（一次系统调用都不做），
  也可能正躺在自己内核栈上跑内核代码。前者即使你把 pending 位置上了，它也永远不会
  自己发现；后者你更不能从别的 CPU/中断里改它的帧。Linux 的解法是“**记下来，出门再送**”：
  `send_sig()` 只置 `TIF_SIGPENDING`，真正的动作在 `exit_to_user_mode_loop()` ——
  **回用户态的最后一步**才 `get_signal()`，要么调 handler，要么按默认动作自杀。
  本版把这一段照抄成了两个函数：

  ```
  task_signal(pid, sig)           // IRQ1 里按 ^C、用户态 kill()、内核里 kill 都走它
      -> t->sig_pending |= 1u << sig     // 只置位，不打断任何人

  task_check_signals(r)           // interrupt_dispatch 出口（syscall 后 / IRQ 后 / 缺页修好后）
      -> 取 pending & ~mask
      -> SIGINT/SIGTERM/SIGKILL（且没被屏蔽）：task_terminate_by_signal(sig)
            退出码 = 128 + sig，term_sig = sig，notify_parent()，schedule_away()
      -> 其他号：本版没有默认动作，丢掉（Linux 里 SIGCHLD 默认是 ignore）
  ```

- **`term_sig` 与 `exit_code` 分开记**，对应 Linux 的 `WIFSIGNALED` / `WTERMSIG`：
  正常退出 42 和“被 SIGINT 杀掉”最终都要经过 `wait()`，但父进程必须能分辨这两件事。
  本版用“退出码 128+sig + term_sig”这一对来表达（和 shell 的 `$?` 约定一致）。

- **`waitpid(pid)`**（`SYS_WAITPID 21`）：`wait()` 是“随便哪个孩子”，`waitpid` 是“就等这一个”。
  实现上就是给原来那段循环加一层 pid 过滤：先扫僵尸（命中就收尸返回），再扫活着的
  （命中就 `TASK_WAIT` 睡下，子进程退出时把 code 写回**本帧的 rax**）——
  M7 那套“用中断帧当返回值载体”的技巧原样复用，没有新增机制。

- **Ctrl-C 走的是真中断，不是“shell 自己读到一个 ^C 字符”**：

  ```
  IRQ1 (键盘)
    keyboard_handle_irq()      // 驱动里 Ctrl 按下位；此时按下 c 不产生字符，只置 ctrl_c_flag
    pic_send_eoi(irq)          // 坑：EOI 必须先发（第一版写成 return 在前，键盘整条线被屏蔽）
    keyboard_take_ctrl_c()     // 有 ^C 事件 ->
    task_signal_foreground(SIGINT)   // 置前台进程的 pending 位
  ...回到 interrupt_dispatch 出口 -> task_check_signals() -> 前台进程在下一次
     回 ring3 时被 128+2 = 130 杀掉
  ```

  “前台是谁”这件事，Linux 用进程组 + 控制终端的 `tcsetpgrp()` 表达；本版是 TCB 上的一位
  `foreground`，由 ring3 shell 在 `fork()` 之后、`waitpid()` 之前用 `SYS_SETFG` 设置，
  收尸之后用 `SYS_SETFG(-1)` 清空 —— 这样在提示符上按 ^C 不会误伤上一个已经退出的进程。

- **一屏里能看到什么**（`sig` 命令 / `build/qemu_verify_m17.sh` 第 1 屏）：

  ```
  [zh@zh_OS ~]$ sig
  M17 signals: kill() only marks a bit; delivery happens on the way back to ring3
    created pid=0 (it has not run a single instruction yet)
    kill(0, SIGKILL) -> queued (pending bit set, nobody interrupted)
  [kernel] pid 0 terminated by SIGKILL (9), exit code 137
    killed    : term_sig=9  exit_code=137  sent=1  delivered=1
    demo      : pid=1 (fork -> kill(child,SIGKILL) -> waitpid)
  [sigdemo] parent: forked child pid=2, sleeping 30 ticks (300 ms)
  [sigdemo] child: spinning in ring3, no syscalls at all...
  [sigdemo] parent: kill(child, SIGKILL) -> queued
  [kernel] pid 2 terminated by SIGKILL (9), exit code 137
  [sigdemo] parent: waitpid(2) = 137   (128 + 9 = 137 means "killed by SIGKILL")
  M17 PASS
  ```

  第 1 段最值得看：进程**还没执行过一条指令**，SIGKILL 就已经 pending 了；它第一次
  从内核进 ring3 时就被打死 —— 说明送达点是“内核→用户”的边界，不是“某个循环里查一下”。
  第 2 段是 ring3 里父子双方演的同一件事，子进程全程自旋（`for(;;){}`，零系统调用）。

- **第 2 屏（Ctrl-C 真按下去）**：`sh3` 里跑 `/bin/spin.elf`（纯自旋、没有 syscall），
  脚本发 `sendkey ctrl-c`，屏幕上是：

  ```
  [kernel] ^C -> SIGINT queued for the foreground process
  [kernel] pid 1 terminated by SIGINT (2), exit code 130
  [spin killed by signal 2 SIGINT, status 130]
  zh3:/bin$ _            <- shell 还在，提示符照旧可用
  ```

  这就是作业控制的第一块砖：**按时钟中断把信号送进一个根本不在内核里的进程**。

- **验收**：`bash build/qemu_verify_m17.sh`（3 屏：`sig` / `^C` / ^C 之后 `ls /bin` 仍正常），
  第 3 屏顺带回归了 M14（`(15 entries, read one by one through SYS_READDIR)`，
  程序从 13 个涨到 15 个）。命令表 44 → **45 条**（新增 `sig`）。
  ISO 5,787,648 字节（M16 版 5,769,216，+18,432 = spin.elf 5064 + sigdemo.elf 6224 + 命令与代码）。

- **还没做的（M18 的活）**：用户态 `signal(sig, handler)` + `sigreturn`（现在只有默认动作，
  没法“捕获”）；`SIGSTOP`/`SIGCONT` + `fg`/`bg`/`jobs`（有了它们才有真正的作业控制）；
  信号屏蔽字 `sigprocmask`（字段 `sig_mask` 已经留好了）。

### 信号捕获 + 作业控制 M18（已实现）

M17 的信号只有三个默认动作（死、忽略、丢弃）。M18 补上 Linux 信号机制真正的那一半：
**用户可以接管**（`signal()` + `sigreturn`），以及**作业控制**（`SIGSTOP` / `SIGCONT` / `^Z` / `jobs` / `fg` / `bg`）。

#### 1) 用户 handler：内核要伪造一次"函数调用"

handler 是用户自己的函数，但它是被**中断**硬插进去的，没有任何人 `call` 过它。
Linux 干的事（`setup_rt_frame`）分三步，本版一模一样：

```
1) 把当前整帧（22 个 qword：15 个通用寄存器 + vector/error + rip/cs/rflags/rsp/ss）
   存进 TCB 的 sig_frame[22]                     ← 这是"回家"的凭据
2) 在**用户栈**上压一个返回地址，值是蹦床页地址    ← 内核伪造的"调用者"
3) rip = handler、rdi = 信号号（handler(sig)）、rax = 0
   然后 iretq —— CPU 就这么"跳进"了用户函数
```

handler 最后那条 `ret` 弹出第 2 步压的地址 → 落进**蹦床页**（
`VA = VMM_USER_BASE + VMM_USER_SPAN - 2 * 4096`，即栈页下面那一页）：

```
b8 18 00 00 00      mov eax, 24      ; SYS_SIGRETURN
cd 80               int  0x80
```

`SYS_SIGRETURN`（24 号）把 `sig_frame` 原样扣回中断帧，再 iretq —— 于是程序从
**被打断的那条指令**继续，寄存器、栈指针全都在原位。glibc 的 `__restore_rt`
用的就是同一招（它调 `rt_sigreturn`），区别只是那段代码在 vDSO 里，不在进程自己栈下。

- 蹦床页**延迟创建**：第一次真要送 handler 时才 `phys_alloc_frame()` + `vmm_map_4k`
  （记在 TCB 的 `tramp_pa`），fork 出来的子进程靠 `vmm_space_clone` 复制页自然继承。
- handler 期间 `sig_mask |= 1<<sig`（Linux 的 `sa_mask` 最小版），
  `sig_in_handler` 挡住嵌套（只有一格 `sig_frame`，嵌套会覆盖掉回家的路）。
- **不可捕获**：`SIGKILL` / `SIGSTOP`（`SYS_SIGNAL` 直接返 -1，和 Linux 一致）。

#### 2) 送达点：只在"回用户态"那一帧动手（这轮最大的坑）

```
[kernel] SIGINT -> entering the user handler 之后立刻
KERNEL PANIC: General Protection Fault, rip = 0x...1017ab (iretq), cs = 8
```

原因：键盘 IRQ 完全可能在进程**陷在内核里**的时候到来（`SYS_SLEEP` 里的 `hlt` 循环就是）。
那时 `r` 是内核里那层的中断帧（cs=0x08、rsp 是内核栈），照它去改 rip/rsp，
iretq 就拿着内核的 cs 去跳用户地址。Linux 早就把这个位置定死了：
`exit_to_user_mode_loop()` 是**唯一**的 `get_signal()` 点。本版用一句
`if ((r->cs & 3) != 3) return;` 表达同一件事 —— pending 位留着，
等 syscall / IRQ 一路退回用户态边界再送。

#### 3) 停下与继续：`TASK_STOP` + `pick_next` 跳过

- `enum task_state` 新增 `TASK_STOP = 5`；`pick_next()` 只挑 `TASK_READY`，
  所以被停住的进程**自动**不上 CPU —— 不需要额外判断。
- `task_stop_current(sig, r)`：标 `TASK_STOP`、`term_sig = sig`、`foreground = 0`、
  叫醒等它的父进程（写回 `0x10000 | sig`，这就是 Linux `WUNTRACED` 的语义），
  再 `schedule_away()`。**停住 ≠ 退出**：不收尸、不释放槽位、`exit_code` 不动。
- 这里必须 `cur->frame = (uint64_t)r`：`schedule_away()` 靠 TCB 上的 frame 做 iretq，
  不记下来，SIGCONT 醒来就会跳进一个旧现场（实测过：`fg 1` 之后立刻 #GP）。
- `SIGCONT` 不能只置 pending 位：被停住的进程根本不会再跑到任何检查点。
  所以 `task_signal()` 里对 `TASK_STOP` 的进程**直接** `task_continue_pid()`
  放回就绪队列 —— 就是 Linux 的 `signal_wake_up_state()`。

#### 4) 三个新系统调用

```
SYS_SIGNAL    24  (int sig, uint64 handler) -> 旧 handler；SIGKILL/SIGSTOP 返回 -1
SYS_SIGRETURN 25  ()                       -> 0 恢复成功；不在 handler 里 -1
SYS_GETSTATE   26  (int pid)               -> 0 FREE 1 READY 2 RUNNING 3 DEAD 4 WAIT 5 STOP
SYS_WAITPID   21  (int pid, uint32 flags)  -> flags: bit0 WNOHANG, bit1 WUNTRACED
                                              被停住返回 0x10000 | sig
```

`SYS_GETSTATE` 相当于 `/proc/<pid>/stat` 的第一个字母：`jobs` 用它判断
"还活着吗、是停着还是跑着"；`WNOHANG` 用它判断不了的事（比如退出码 0）
才走 `waitpid`。`SYS_SIGRETURN` 的号写死在蹦床页的机器码里，改号必须同时改
`task_install_trampoline()`。

#### 5) ring3 shell 的作业表（`user/zhsh3.c`）

```
static job_t g_jobs[4];   /* used / pid / state / last_code / name[24] */
```

- `run_command()`：fork 后登记作业 → `SIGTSTP` 来了由内核停住 → 父进程的
  `waitpid(pid, WUNTRACED)` 拿到 `0x10000|20`，打印 `[1] Stopped   jobdemo`，
  **作业留在表里**（进程还在内存里）。
- `jobs`：逐个 `SYS_GETSTATE`，DEAD 的顺手 `waitpid(WNOHANG)` 收尸并标 Done，
  `TASK_STOP` 的显示 Stopped，其余 Running。
- `fg <n>`：`kill(pid, SIGCONT)` → `SETFG` → `waitpid(pid, WUNTRACED)`；
  `bg <n>`：只发 `SIGCONT`，不 wait（回提示符）。
- `^Z` 由键盘驱动识别（`Ctrl+Z` 不产生字符，只置 `ctrl_z_flag`）→
  `isr.c` 的 IRQ1 里 `task_signal_foreground(SIGTSTP)`。

#### 6) 演示程序

- **`/bin/sigcatch.elf`（`user/sigcatch.c`）**：`signal(SIGINT, on_sigint)` 之后进
  200 轮 ×50ms 的循环（每轮打一个 `.`）。按 ^C 之后屏幕上出现
  `>>> inside my own handler, sig=2 (this is user code at ring3, not the kernel)`，
  handler `ret` → 蹦床 → `sigreturn`，循环从 **counter=65 那一轮**接着跑，
  最后 `exit(42)`，shell 打 `[sigcatch exited 42]`。
- **`/bin/jobdemo.elf`（`user/jobdemo.c`）**：每 100ms 数一个 tick，每 20 个打一行。
  `^Z` 停住（tick 停住）、`fg 1` 继续（tick 从 41 接到 60）、`^C` 杀掉（130）。

#### 7) 验收

```
bash build/qemu_verify_m18.sh
  screen 1: sigcatch —— signal(SIGINT, handler) + sigreturn
  screen 2: ^Z       —— 前台作业被 SIGTSTP 停住，jobs 里是 Stopped
  screen 3: fg 1     —— SIGCONT 继续 + ^C 杀掉（130）
-> M18 PASS: a user handler runs in ring3, sigreturn restores the frame,
             and ^Z / jobs / fg move a process between running and stopped.
```

`bash build/regress_all.sh` 覆盖 M2 … M18。ISO 从 5,787,648 涨到
**5,808,128 字节**（sigcatch.elf + jobdemo.elf + 蹦床页/作业表代码）。
`/bin` 里现在有 **17 个程序**。

#### 8) 这轮踩到的一个纯 C 坑（值得单独记）

`task_create_elf()` 里加了一段清零 handler 表的循环，用的是函数原来的循环变量 `i`，
加完后面还留着老的 `return (int)i;` —— 于是它返回 **32** 而不是 0。

后果极诡诈：fork 出来的 `sigcatch` 拿到 pid 32，父进程 `waitpid(32)` 立刻 ECHILD；
更糟的是 `sh3` 这个 ring-3 shell 拿到的 TCB 是 NULL，M4–M17 的 verify 全都没有 PASS 行
（只打 `state=??` / `died with a fault`），看着像"早期里程碑集体回归"，实际是一个返回值。

教训：函数尾部只要 `return i`，中间任何新加的循环都必须换变量；更稳的写法是
`return t->pid;` / `return child->pid;`（fork 与三条 create 路径现在就都是这么写的）。

### 块设备上的 ext2 只读挂载 M19（已实现）

到 M18 为止，内核里的文件全都在内存里：`ramfs`（`touch` 写进去的）和 `initrd`（启动时把嵌入
的 17 个 ELF 铺进 `/bin`）。掉电就没了。M19 把"真正的磁盘文件系统"接进来 ——
**读的是 ATA 主通道主设备（QEMU `-drive if=ide,index=0`）上一张真 ext2 镜像**。

#### 1) 三层结构，和 Linux 的调用链一一对应

```
用户:  ext2cat /docs/note.txt
  ↓  shell/cmd_ext2.c          参数 + 错误码翻译
fs:    fs/ext2.c               超级块 -> 块组描述符 -> inode 表 -> inode -> 目录项 -> 数据块
块层:  drivers/ata.c           ata_read_sector(lba, buf)   （B2 的 PIO 驱动，一行未改）
硬件:  0x1F0 端口 LBA28 + PIO
```

Linux 那边是 `ext2_readpage()` → `sb_bread()` → `submit_bh()` → 块设备驱动；
这里少了缓冲层和 BIO，把 `sb_bread()` 直接写成了 `read_block_to()` —— 一块 = `block_size/512`
个扇区，一扇区一扇区地要。

#### 2) 挂载：只做三件事

1. **超级块**：绝对偏移 1024（扇区 2、3 拼出 1024 字节），`s_magic` 必须是 **0xEF53**，
   否则报 `not an ext2 filesystem (magic != 0xEF53)`；
2. **块大小**：`bs = 1024 << s_log_block_size`（1024 / 2048 / 4096 都支持）；
3. **块组描述符表**：紧跟在 `s_first_data_block + 1` 那个块后面，第 0 组的
   `bg_inode_table`（偏移 8）就是 inode 表的起始块号。

`ext2info` 把这几项摊开打印，和宿主上的 `dumpe2fs -h` 逐项对得上：

```
magic      : 0xef53
block size : 1024 bytes   blocks=16384
inodes     : 4096   first_ino=11   inode_size=256
per group  : blocks=8192  inodes=2048
gdt block  : 2   inode table block=68
```

#### 3) inode 定位（ext2 的核心公式）

```
组号   group = (ino - 1) / inodes_per_group
组内号 index = (ino - 1) % inodes_per_group
inode 在表里的偏移 = index * inode_size
读哪一块 = bg_inode_table + 偏移 / block_size
```

`ino == 2` 永远是根目录（ext2 把根 inode 写死在 2 —— Linux 里就是 `EXT2_ROOT_INO`）。
inode 里只取四样：`i_mode`（判断目录/普通文件）、`i_size`、`i_block[0..14]`、跨块时按
`inode_size` 拼两块。rev0 的 inode 是 128 字节，rev1 由 `s_inode_size`（本例 256）决定。

#### 4) 目录项与文件名

ext2 目录内容不是"名字列表"，而是一串变长记录：

```
偏移 0  inode (4)   偏移 4  rec_len (2)   偏移 6  name_len (1)   偏移 7  file_type(1，可选)
偏移 8  名字(name_len 字节)，剩余 rec_len - 8 - name_len 是填充
```

`file_type` 那个字节是否存在，取决于 `s_feature_incompat` 的 **FILETYPE(0x2)** 位 ——
**这一位决定了名字从偏移 7 还是 8 开始**。本版按这一位算 `g_name_off`，两个分支都写了，
否则遇到老 ext2 镜像名字会整体错一位。`rec_len` 必须让 `off += rec_len` 最后正好走到块尾，
非法就直接报 `corrupt inode or directory entry`，不做"继续猜"。

`ext2ls` 跳过 `.` 和 `..`，目录亮蓝带 `/`、文件浅灰、每行 4 个 —— 和 `fatls` 同一套视觉语言。

#### 5) 读文件：直接块 → 一级间接 → 二级间接

```
i_block[0..11]  12 个直接块        → 前 12KB（bs=1024 时）
i_block[12]     一级间接块         → 再 256KB
i_block[13]     二级间接块         → 再 64MB
```

`copy_from_pointer_block()` 用一个 `level` 参数表达两级（2 → 递归进 1），这就是 ext2 的
间接块树。验收里那张 `big.txt` 是 **22,420 字节**，早就过了 12 个直接块；
屏幕上先打 `ext2cat: /big.txt (22420 bytes)`，滚过 400 行之后**最后一行是
`TAIL-MARKER-18KB-OK`** —— 说明第 13 块之后的字节确实是从一级间接块里读出来的。

#### 6) 四个新命令（命令表 45 → 49 条）

```
ext2mount            挂载主通道主设备上的 ext2（失败会给具体原因）
ext2info             打印超级块几何（magic / block size / inode_size / inode table）
ext2ls [path]        列目录；不存在 / 不是目录 / 未挂载 分别报不同错误
ext2cat <path>       打印文件；>32KB 截断并亮黄提示
```

ext2 的命令与 ramfs 的 `ls/cat`、FAT16 的 `fatls/fatcat` **互不覆盖**：
三套文件系统同屏并存，各自读各自的那块盘。

#### 7) 验收

```
bash build/mk_ext2.sh            # 造 16MB ext2 镜像 + 5 个测试文件（宿主上用 mke2fs/debugfs）
bash build/qemu_verify_m19.sh    # QEMU 挂 -drive if=ide,index=0，3 屏
  screen 1: ext2mount + ext2info —— 超级块 -> 块组描述符
  screen 2: ext2ls / + ext2ls /docs + ext2cat /docs/note.txt + 不存在路径报错
  screen 3: ext2cat /big.txt（22KB，走一级间接块，末尾 TAIL-MARKER 可见）
-> M19 PASS: the kernel walked ext2 from its superblock down to a 22KB file
             (block group -> inode table -> inode -> directory entry -> indirect block).
```

### 统一命名空间：VFS 挂载表 M20（已实现）

M19 之后栈里已经躺着三套文件系统：`ramfs`（内存）、`fat16`（从盘）、`ext2`（主盘）。
它们各自有一套命令（`ls` / `fatls` / `ext2ls`），用户得自己记住"我现在在哪个盘上"。
Linux 不是这样的：**一个 `/`，不同的子树挂不同的文件系统**。M20 把这件事补上。

#### 1) 只保留两样真正起作用的东西

Linux 的 VFS 有 `super_block`、`vfsmount`、`dentry`、`inode`、`file_operations` 一大套；
这里只做两条：

```
struct vfs_mount {  char prefix[32];  vfs_fs_t fs;  used;  }   // 挂载点 + 谁在上面
vfs_resolve(path) -> { 挂载项, 该 FS 内的路径 }                // 最长前缀匹配
```

以及一个 `vfs_list / vfs_read` 的分发层 —— 相当于 `file_operations` 的间接调用，
这里用 `switch (fs)` 代替函数指针表（freestanding、没有虚表也够用）。

#### 2) 最长前缀匹配（代替 dentry 树向上回溯）

```
ls /mnt/docs/note.txt
 -> 规范化：折叠 "//"、去掉 "./"、处理 ".."                     （normalize_abs）
 -> 相对路径先拼上 cwd（Linux 的 nd->path 也是从 cwd 开始的）
 -> 在挂载表里挑"最长的、能当路径前缀的"那一个：/mnt
 -> 剩下的 /docs/note.txt 交给 ext2；ext2 那边的根就是 /mnt 的根
```

`prefix_match()` 要求前缀之后正好是 `'\0'` 或 `'/'` —— 否则 `/mntx` 会被 `/mnt` 误命中，
这是挂载点匹配里最经典的一个坑。

#### 3) 没挂载点匹配上 -> 原样回落 ramfs（纯增量的关键）

`vfs_resolve` 找不到挂载点就返回 0，`vfs_list/vfs_read` 返回 `VFS_ERR_NOENT`；
`cmd_ls` / `cmd_cat` 看到这个码就走 **M1 以来的那段老代码**，连颜色、列宽、每行 4 个都没变。
所以挂载表空着的时候（也就是开机默认状态），`ls` / `cd` / `pwd` / `cat` 的行为与 M19 完全一致 ——
M13/M14 那些验收脚本逐字节比对照样过。

#### 4) cwd 归谁：ramfs 还是 VFS

`ramfs` 的目录树里根本没有 `/mnt` 这个节点，所以 **挂在挂载点里的 cwd 由 VFS 自己持有**：

```
cd /mnt/docs       -> vfs_set_cwd("/mnt/docs")   （ramfs_chdir 不会被调用）
pwd                -> vfs_get_cwd() = /mnt/docs  （ramfs_get_cwd 会返回 "~"，不对）
ls / cat note.txt  -> 相对路径按 VFS 的 cwd 拼接，再走最长前缀匹配
cd ..              -> vfs_join_cwd() 砍掉末尾一段 -> /mnt
cd /               -> vfs_under_mount("/") 不成立 -> ramfs_chdir("/")，VFS cwd 退回根
umount /mnt        -> 如果 cwd 就在被卸的挂载点里，cwd 自动退回 "/"
```

#### 5) 三个新命令（命令表 49 → 52 条）

```
mount <ext2|fat> </dir>   先把底层 FS 挂上（ext2mount / fatmount 那一步），再登记 vfsmount
umount </dir>             从挂载表里摘掉
mounts                    打印挂载表；空表时明确说 "every path goes to ramfs"
```

顺序和 Linux 一样：**先有 superblock，再有 vfsmount**。

#### 6) 验收

```
bash build/qemu_verify_m20.sh        # QEMU 挂 ext2.img（if=ide,index=0），3 屏
  screen 1: mounts 空表 -> mount ext2 /mnt -> mounts -> ls /mnt -> cat /mnt/hello.txt
  screen 2: cd /mnt/docs -> pwd(/mnt/docs) -> ls / cat note.txt -> cd .. -> pwd(/mnt)
  screen 3: umount /mnt -> ls /mnt 报红（已经回落 ramfs，"没有这个目录"）
-> M20 PASS: one namespace -- /mnt is ext2, everything else is ramfs,
             no code in user space changed.
```

`ext2ls` / `ext2cat` / `fatls` / `fatcat` 一个没删：它们现在是"绕过 VFS 直连调试"用的，
就像 Linux 里 `debugfs` 之于 `mount`。

### ext2 可写：位图分配与建文件 M21（已实现）

M19 让内核**读得懂** ext2，M20 把它挂进了统一的 `/`。但只能读。M21 开始写：
让内核自己在块位图和 inode 位图上分配资源，写数据块、写 inode、往父目录插目录项。
能不能算"写成功了"，不看内核自己的 `ext2cat` —— 要拿宿主上的 `debugfs` 和 `e2fsck` 来验。

#### 1) 写的基本单位：一个块

`read_block_to()` 的镜像版本 `ext2_write_block()`：一个 ext2 块 = `block_size/512` 个扇区，
逐个 `ata_write_sector()` 写下去。**write-through，没有页缓存，所以也就没有 fsync**
（Linux 那边 `ext2_sync_inode()` 是为了把脏页交出去，这里每改一个字节立刻落盘）。

#### 2) 分配器：块位图与 inode 位图

```
块:   bit = (块号 - s_first_data_block) % blocks_per_group
      组   = (块号 - s_first_data_block) / blocks_per_group
      alloc: 在 bg_block_bitmap 指向的块里找第一个 0 位，置 1
inode: 组   = (ino - 1) / inodes_per_group
      bit = (ino - 1) % inodes_per_group
      alloc: 在 bg_inode_bitmap 里找第一个 0 位，组 0 要跳过 1 号与 s_first_ino 之前的保留项
```

分配完要**同时改三处**，缺一个 `e2fsck` 就会报：

1. 位图本身（置 1）
2. 组描述符里的 `bg_free_blocks_count` / `bg_free_inodes_count`（减 1，建目录还要 `bg_used_dirs_count` 加 1）
3. 超级块的 `s_free_blocks_count` / `s_free_inodes_count`（减 1）

`ext2free` 每次重新读超级块打印这两个数，所以"写一个 32 字节文件"在屏幕上的证据是：

```
free blocks 15183   free inodes 4079      <- 写之前
created /zhnew.txt  inode 18  32 bytes written to disk
free blocks 15182   free inodes 4078      <- 写之后（各减 1）
```

#### 3) `i_block[]` 的写版本（bmap）

Linux 的 `ext2_get_block()` 要处理"逻辑块号 → 物理块号"的全部三级映射。这一版只做
**12 个直接块**（`<= block_size*12` 字节，bs=1024 时 12KB），每个逻辑块现分配一个物理块：

```
for 每个逻辑块:
     alloc_block()  -> 物理块号
     写满这个块（尾部补 0，不留磁盘上的旧垃圾）
     inode->i_block[n] = 物理块号
```

间接块的写（`i_block[12]` 分配一个指针块、再写它指向的数据块）留到写大文件那一步 ——
读路径 M19 已经能走间接块了，写路径补齐只是"多分配一层块"。

#### 4) 建文件的四步（和 Linux `ext2_create` 同构）

```
1. alloc_inode()                     -> 拿到 ino，位图 + 两处计数都更新
2. 分配数据块并写内容                -> i_block[0..n]
3. 把 inode 写进 inode 表            -> 满 256 字节先清零，只填 mode/size/links/i_blocks/
                                        时间戳/i_block[]（零填充很重要：新 inode 位置
                                        可能是别人用过的，不清零会把旧值带进来）
4. add_dir_entry()                   -> 父目录里插一个目录项
```

第 4 步就是 `ext2_add_entry()` 的核心技巧 —— **拆 `rec_len`**：

```
原来:  [ inode=A | rec_len=1012 | name=".." ]
需要:  插入 name="zhnew.txt"，新项长度 need = 8+10 向上取整 = 20
做法:  [ inode=A | rec_len=12 | name=".." ][ inode=18 | rec_len=1000 | name="zhnew.txt" ]
       把前一项的 rec_len 砍到它自己实际用的长度，剩下的空间正好让新项落脚
```

目录一个块都没有时（`block[0]==0`）先 `alloc_block()` 一个整块，并把父 inode 的
`i_size` 加上一个块大小 —— 这就是"目录扩容"。

#### 5) 两条健壮性约定

- **同名冲突**：建之前先 `dir_lookup()`，有同名直接报 `file exists`（屏幕上红字可见）。
- **只支持直接块**：超过 12 块就报 `no space left on the device`，
  不会去写坏间接块 —— 宁可少支持，也不写出一张 `e2fsck` 会骂人的盘。

#### 6) 验收（三重证据）

```
bash build/qemu_verify_m21.sh
  screen 1: ext2free(15183/4079) -> ext2create /zhnew.txt -> ext2free(15182/4078)
  screen 2: ext2cat /zhnew.txt 读回来 + ext2ls / 里出现 zhnew.txt + 再建同名报 file exists
  screen 3: ext2cat /hello.txt、/docs/note.txt —— 旧文件一个字节没坏
宿主侧（完全不经过内核）:
  debugfs -R "ls -l /"        -> 18  100644 (1) 0 0 32  zhnew.txt
  debugfs -R "stat /zhnew.txt"-> Inode: 18  Mode: 0644  Size: 32  Blocks: 2
  debugfs -R "cat /zhnew.txt" -> written by the zh kernel itself
  e2fsck -fn                 -> 18/4096 files, 1202/16384 blocks，五个 Pass 全过、0 错误
```

`e2fsck` 一句错都不报，才是"内核真的写出一张合法 ext2"，而不是"内核自己读自己写的缓存"。

### 间接块写：让文件越过 12KB M22（已实现）

M21 的写路径只有 12 个直接块，超过 12KB 就 `no space left on the device`。
M19 的读路径早就会走 `i_block[12]` 了 —— 这一步把写的对称面补上，
于是"内核能写出的文件"从 12KB 涨到 268KB（`block_size*(12 + block_size/4)`）。

#### 1) 一次分配两种东西

```
12 个直接块:   i_block[0..11]  每个槽一个数据块
一级间接:      i_block[12]  ->  指针块（里面 256 个 u32，每个指向一个数据块）
```

指针块里那 256 个槽 = `block_size / 4`（bs=1024 时 256）。所以理论上限是
`12 + 256 = 268` 个块，bs=1024 时 268KB；这也就是 Linux `ext2_get_block()` 里
`add_branch()` 做的事 —— 先分配一个"枝"，再往枝上挂叶子。

#### 2) 攒完再写指针块

数据块是一个一个分配、一个一个写的，但指针块**最后才写一次**：

```
for 第 13..N 块:
      phys = alloc_block()
      write(phys, data)              <- 数据立刻落盘
      g_wptr[slot*4] = phys          <- 指针先攒在内存里
      slot++
write(i_block[12], g_wptr)           <- 一次写回，256 个槽同时生效
```

这样省掉 255 次"改一个槽就写回整块"的重复落盘，但代价是中途崩溃会留下
"数据块已分配、指针块还没写"的半成品 —— 正是 Linux 里 journalled 日志要解决的
问题。教学内核先不碰 journal，把顺序做成"数据先、指针后"已经算对得起 ext2 的良心。

#### 3) `i_blocks` 要算上指针块

`i_blocks` 是**512 字节扇区**的计数，不是块数：

```
i_blocks = (数据块数 + 1(有间接块时)) * (block_size / 512)
```

再写一个 18.4KB 的文件，是 18 个数据块 + 1 个指针块 = 19 个 1024 字节块
= 38 个扇区，`debugfs` 里 `Blockcount: 38` 对得上。少算这一个指针块，
`e2fsck` 的 Pass 5（group summary）就会抓到块计数不符。

#### 4) 新命令 `ext2fill` 为什么存在

`ext2create` 的内容来自命令行，最多 4000 字节，天然到不了 12KB。
所以加一条专门的命令：给一个 KB 数，内核自己在内存里生成等量的正文
（每行 `line 000 of the indirect-block write test`），末尾追加一行
`END-OF-INDIRECT-WRITE` —— 这一行**必然落在第 13 块之后**，
只有指针块写对了，`ext2cat` 和宿主上的 `debugfs cat` 才能把它读出来。
这就是"用内容本身证明间接块生效"，而不是靠打印一个块号自证。

#### 5) 验收

```
bash build/qemu_verify_m22.sh
  screen 1: ext2free 15183/4079 -> ext2fill /big2.txt 18 -> ext2free 15164/4078
            wrote /big2.txt  inode 18  18418 bytes, 18 data blocks, indirect ptr block #634
  screen 2: ext2cat /big2.txt 滚到第 438 行，末行 END-OF-INDIRECT-WRITE
宿主侧:
  debugfs -R "stat /big2.txt"
    Size: 18418   Blockcount: 38
    BLOCKS: (0-11):622-633, (IND):634, (12-17):635-640        <- (IND) 就是 i_block[12]
    TOTAL: 19
  debugfs -R "cat /big2.txt" | tail -1  ->  END-OF-INDIRECT-WRITE
  e2fsck -fn  ->  18/4096 files, 1220/16384 blocks，五个 Pass 全过、0 错误
```

少 19 个块（15183 → 15164），`(IND)` 落在中间，`e2fsck` 一句不报 ——
三条证据互相对得上，才叫"间接块真的写通了"。

### 删除：把块、inode 和目录项都还回去 M23（已实现）

M21/M22 让盘只会变胖。删文件看着简单，其实是**四个独立的归还动作**，
少做任何一个 `e2fsck` 都会留下"孤儿块"或"幽灵 inode"。

#### 1) `free_block()`：位图 + 组描述符 + 超级块

```c
group = (blk - first_data_block) / blocks_per_group;
bit   = (blk - first_data_block) % blocks_per_group;
位图 bit 清 0                     <- bg_block_bitmap
bg_free_blocks_count   + 1        <- 组描述符
s_free_blocks_count    + 1        <- 超级块
```

分配时是"三处一起减"，释放就是"三处一起加"。
这里加了一条 Linux 没有的保险：**位本来就是 0 就报 `corrupt inode or directory entry`**
—— 重复释放是文件系统最容易变成"看起来正常、实则到处是洞"的错，
宁可红字报错，也不要静悄悄把它当成功。

#### 2) `ext2_truncate()`：顺序反了就漏块

```
i_block[0..11]   -> 直接 free_block()
i_block[12]      -> 读指针块，逐个 free 它指向的数据块，再 free 指针块本身
i_block[13]      -> 二级间接：递归一层（free_ptr_tree(level=2)）
i_block[14]      -> 三级间接：本内核从不分配，读到非 0 直接报 corrupt
```

**指针块必须先读完再释放**。如果先 `free_block(ptr)` 再想遍历里面的指针，
那块内容可能已经被下一个分配者覆盖了 —— 这是真实内核里踩过的经典坑。
`free_ptr_tree()` 用 `g_freebuf` 读指针块，而 `free_block()` 用 `g_freebm` 读位图，
**两个缓冲必须分开**（本轮就是这里先写成了共用一个，导致 18KB 文件删到第 13 块时
`ext2rm: I/O error`，因为指针块缓冲被位图内容冲掉了）。

#### 3) `remove_dir_entry()`：`rec_len` 反向合并

建文件时是"把前一项的 `rec_len` 砍小、剩下空间给新项"；删就是反过来：

```
原来:  [ "hello.txt" | rec_len=32 ][ "zhnew.txt" | rec_len=980 ]
删除:  把被删项的 rec_len 加到前一项上
结果:  [ "hello.txt" | rec_len=1012 ]
```

这样一块目录始终**没有碎片**，`e2fsck` 的 Pass 2 才不抱怨。
被删项正好是块里第一项时没有前驱可并，只能把 `inode` 置 0（Linux 也这么处理）。

#### 4) inode 的三件事：`links=0`、`i_dtime`、`i_blocks=0`

```
i_links_count = 0
i_dtime       = 删除时刻（0x14 偏移，不是 0x08！）
i_blocks      = 0
i_block[0..14]= 0
```

`i_dtime` 踩了个偏移的坑：ext2 inode 里 `0x08` 是 `i_atime`、`0x14` 才是 `i_dtime`。
先写成 `0x08` 时 `e2fsck` 报 `Deleted inode 18 has zero dtime` ——
它用"dtime 是否非 0"来判断"这个 inode 是不是被删过"。写错了不报别的，就报这一句。

#### 5) 验收

```
bash build/qemu_verify_m23.sh
  screen 1: ext2free 15183 -> ext2fill /big2.txt 18（15164）-> ext2rm（回 15183/4079）
            removed /big2.txt  inode 18  19 blocks freed
  screen 2: ext2ls / 里 big2.txt 没了、ext2cat 报 no such file、再删一次也报 no such file
  screen 3: 建一个 45 字节小文件（inode 又拿到 18）、删掉、计数回到 15183/4079，
            hello.txt 原样可读
宿主侧:
  e2fsck -fn  ->  17/4096 files, 1201/16384 blocks，**与造镜像时的原始值完全相同**
```

"回到原始值"是这一步最硬的判据：19 个块、1 个 inode、1 个目录项、父目录的
`rec_len` 全部还原，盘和没动过一样 —— 只有链路每一环都还对了才可能。

#### 6) 已知限制

- **不支持删目录**：`ext2rm /docs` 会红字 `is a directory (rmdir is not supported yet)`。
  删目录要额外处理 `.` 和 `..` 的 `nlink`、父目录 `bg_used_dirs_count`，
  以及"目录里还有文件就不许删"的检查，留给下一步。
- **没有硬链接**：`i_links_count` 恒为 1，所以"减到 0 才释放"的完整语义还没用上。

### VFS 写路径：touch 与 rm 落到 ext2 M24（已实现）

M20 的 VFS 只会 `list` / `read`。写请求根本到不了 ext2 —— `touch /mnt/x`
一直是 ramfs 在自己的树里造节点，而 ramfs 里压根没有 `/mnt` 这一层，所以
`ls /mnt` 看不到、重开 QEMU 就消失。M24 补齐 `create` / `unlink` 两个入口。

#### 1) 形状照着 inode_operations 来

```
vfs_create(path, data, len)   ≈  inode->i_op->create()
vfs_unlink(path)              ≈  inode->i_op->unlink()
```

两个函数都先 `vfs_resolve()`：**没匹配到挂载点就返回 `VFS_ERR_NOENT`**，
命令层看到这个错就照旧走 ramfs。这条"回落"是 M20 定下的规矩，
所以挂载表空着时 `touch` / `rm` 的行为一个字节都没变（M13/M14 的逐字节比对仍然过）。

#### 2) 大文件判断要用底层真实的 block_size

```c
if (len > (uint64_t)ext2_block_size() * 12u)
    ext2_create_big(...)     /* 走间接块 */
else
    ext2_create(...)         /* 12 个直接块够了 */
```

不写死 12KB —— 如果哪天镜像改成 4096 字节块，12 个直接块就是 48KB，
写死常量会让大文件被错判成"超限"。

#### 3) FAT16 明确拒绝写

`vfs_create` / `vfs_unlink` 里 FAT16 分支返回 `VFS_ERR_RO`，命令层翻成
`read-only file system`。M8 起 FAT16 这条链路就是只读的，
不假装能写、也不悄悄改去写 ramfs —— 报错比"看起来写成功"诚实。

#### 4) 两种输出风格本身就是证据

```
[zh@zh_OS ~]$ touch /mnt/vfsnew.txt
created /mnt/vfsnew.txt  (inode 18)          <- VFS：能报出 ext2 分配的 inode 号
[zh@zh_OS ~]$ touch /home/zh/fbfile.txt
created: /home/zh/fbfile.txt                 <- ramfs：老的 emit_ok 格式
```

同一台机器上两条路径、两种提示，正好说明请求走的是两套实现。
`inode 18` 这个号也不是随便编的：它来自 `alloc_inode()` 的返回值。

#### 5) 验收

```
bash build/qemu_verify_m24.sh
  screen 1: ext2mount -> mount ext2 /mnt -> ext2free 15183/4079
            -> touch /mnt/vfsnew.txt（inode 18，free inodes 4079 -> 4078）
            -> ls /mnt 里出现 vfsnew.txt
  screen 2: cat /mnt/vfsnew.txt（0 字节，什么都不打印）
            -> rm /mnt/vfsnew.txt（0 blocks freed）-> ls /mnt 里没了
            -> 再 rm 一次报 no such file -> ext2free 回到 4079
  screen 3: touch /home/zh/fbfile.txt -> "created: ..."（ramfs 风格）
            -> rm 掉 -> 回到初始
宿主侧:
  debugfs -R "ls -l /"  -> 没有 vfsnew.txt（真的删干净了）
  e2fsck -fn           -> 17/4096 files, 1201/16384 blocks，无 WARNING
```

`touch` 建的是 0 字节文件：`ext2_create(len=0)` 不分配数据块，只占 inode 和目录项，
所以屏幕上是 `free inodes -1、free blocks 不变`，`rm` 时 `0 blocks freed` —— 这两个数
就是"空文件到底占什么"的直接答案。

#### 6) 已知限制

- **`mkdir /mnt/d` 还不行**：ext2 的 `alloc_inode(is_dir=1)` 已经能正确维护
  `bg_used_dirs_count`，但"写 `.` / `..` 两个目录项 + 父目录 `nlink++`"这一整套
  还没接 VFS，`mkdir` 仍只走 ramfs（在挂载点里会报 `no such file or directory`）。
- **`cat` 空文件不打提示**：`cat` 命令本身没有 `ext2cat` 那种 `(N bytes)` 前缀，
  所以 0 字节文件看起来"没反应"，其实是对的。
- **写只新建、不追加**：`vfs_create` 同名会报 `file exists`，没有 `O_TRUNC` /
  `O_APPEND` 语义 —— 现在还没有 fd 落到 ext2 上。
### 图形模式与分辨率：framebuffer 控制台 M25（已实现）

前面 24 步的屏幕一直是 VGA 文本模式：显存固定在 `0xB8000`，一个字符 2 字节，80×25。
这个模式的"分辨率"是硬件写死的（QEMU 下永远是 720×400），所以在文本模式里谈"调分辨率"没有意义。

M25 换掉的是**显示后端**：让 GRUB 把显卡切到图形模式（VBE），内核从 Multiboot2 里拿到
framebuffer 的地址、宽高、行跨距、色深，再**自己用点阵字体把字一个个像素画上去**。
这就是 Linux 的 `fbcon` 干的事。做完之后：

* 屏幕尺寸不再写死，文本行列是算出来的：`列 = 宽/8`、`行 = 高/16`
* 有了运行时改模式的能力（`res 1024x768`），不用重启换分辨率
* 内存里没有 `0xB8000` 那块字符缓冲了，一切都走线性帧缓冲

#### 1) 为什么要新写一套字符渲染

文本模式下"写字"是往 `0xB8000` 放一个编码，字符形状由显卡内的字符发生器画。
图形模式下没有字符发生器，显存里全是像素，一个字符要自己拼出来：

```
像素 (x0 + x, y0 + y) = 字模第 y 行的第 x 位 ? 前景色 : 背景色
```

字模来自 Linux 控制台字体 `Lat15-Fixed16.psf.gz`（实际是 PSF1：magic `36 04`、
`charsize = 16`、count 字段为 0 表示 256 个字形）。`build/mkfont.py` 把它转成 C 数组：

```
python3 build/mkfont.py /usr/share/consolefonts/Lat15-Fixed16.psf.gz drivers/font8x16.c
-> wrote drivers/font8x16.c: 95 glyphs
```

`drivers/font8x16.c` 里是 `const uint8_t g_font8x16[95][16]`，下标 = `ASCII - 32`，
每个字形 16 字节、每字节是该行 8 个像素（最高位在最左）。抽 `'Q'` 出来验证过：
`0x00,0x00,0x3C,0x42,...0x5A,0x66,0x3C,0x03,...` —— 中间那圈 `0x42` 就是 O 的左右两竖，
`0x5A`/`0x66` 是 Q 的斜脚。

#### 2) 三个新文件

| 文件 | 内容 |
| --- | --- |
| `drivers/font8x16.c` | 95 个 8×16 字形（`build/mkfont.py` 生成） |
| `drivers/font8x16.h` | `FONT_W 8` / `FONT_H 16` / `FONT_FIRST 32` / `FONT_LAST 126` |
| `drivers/fb.c` / `fb.h` | 标签解析、帧缓冲映射、清屏/写字符/滚屏、`res` 换模式 |

`drivers/` 本来就在 `build.sh` 的自动扫描列表里，所以这两支源码不用再 `SOURCES+=`。

#### 3) 从 Multiboot2 拿到屏幕

`boot/boot.asm` 的头里加了一个帧缓冲请求标签（type = 5），宽高色深都写 0：

```
    dw 5                        ; 帧缓冲标签
    dw 0                        ; flags
    dd 20                       ; size
    dd 0                        ; width  = 0：不指定
    dd 0                        ; height = 0：不指定
    dd 0                        ; depth  = 0：不指定
```

**关键点：宽高写 0**。写死一个（比如 1024/768/32）GRUB 只会照办那一个，菜单就再也没法
换档了；写 0 表示"你说得算"，GRUB 自己挑一个它拿得到的模式，然后在信息结构里塞一个
type = 8 的标签告诉内核"我给你的是这个"。

`kernel/kernel.c` 的 `kernel_main` 本来就有 `mb2_info`（`boot.asm` 第 51 行
`mov [mb2_info], ebx` 存下来的），M25 在 `phys_init` 之后接上：

```c
phys_init(mb2_magic, mb2_info);
(void)fb_init(mb2_info);   /* [ADD] M25 */
```

`fb.c` 里沿着标签链表走（每个标签 8 字节头，按 8 字节对齐跳到下一个），
碰到 `type == 8` 读出：

```
framebuffer_addr(u64)  pitch(u32)  width(u32)  height(u32)  bpp(u8)  type(u8)
```

只要 `type == 1`（RGB 直色）且色深是 32/24/16 就接。**不是这种情况就返回 0**，
调用方继续用 `0xB8000` —— 这样 ISO 在没有图形环境的机器上照样能起来。

#### 4) 帧缓冲不在低 1GB：补一段 2MB 大页映射

`paging_init()` 只恒等映射了低 1GB，而 QEMU 的 LFB 在 `0xFD000000`（约 3.95GB）。
不动页表，写第一个像素就是 #PF。`kernel/paging.c` 末尾追加：

```c
int paging_map_2mb_range(uint64_t phys, uint64_t bytes);
```

做法跟 Linux 的 `ioremap()` 一个思路：

* 按 2MB 粒度算 `PML4[i3]` / `PDPT[i2]` / `PD[i1]`
* 缺 PDPT 就填 `PDPT[idx]` 指向一张新页目录，缺页目录就从 `.bss` 里的
  `pd_pool[8][512]` 静态池拿一张（内核镜像在 1MB 处、恒等映射着，所以
  `VA == PA`，指针能直接当物理地址填进页表）
* 每一项填 `addr | PRESENT | WRITABLE | HUGE`，写完 `invlpg` 刷 TLB

**为什么可以偷懒用静态池**：这一步只需要映射一两块设备内存，用不着通用 `mmap`
（要拆页、要引用计数、要找 VMA）。等以后要做得像 Linux 的 `ioremap` / `vmalloc`，
再换成从页帧分配器拿页表。

#### 5) 输出后端切换：只改两个原语

`drivers/vga.c` 里所有输出最后都落到 `vga_clear` 和 `vga_putchar` 这两个函数
（`vga_print` / `vga_println` 只是循环调 `vga_putchar`）。所以 M25 在这两个函数开头各加
一个转发，**所有调用方一个字都不用改**：

```c
void vga_clear(enum vga_color fg, enum vga_color bg)
{
    if (fb_active()) { fb_clear(fg, bg); return; }   /* [ADD] M25 */
    ...
}

void vga_putchar(char c, enum vga_color fg, enum vga_color bg)
{
    if (fb_active()) { fb_putchar(c, fg, bg); return; }  /* [ADD] M25 */
    ...
}
```

颜色也要转一道：文本模式是 4 位调色板索引，图形模式要 24 位 RGB，
`fb.c` 里有一张 16 项的 VGA→RGB 表（黑 0x000000、浅灰 0xAAAAAA、亮绿 0x55FF55 ……），
观感跟原来一致。

滚屏也自己实现：把 `第 1 行..最后一行` 整体往上搬 16 个像素行（`memmove` 式逐字节搬），
最后一行用背景色填满。

#### 6) `videomode`：屏幕几何在哪看

```
[zh@zh_OS ~]$ videomode
framebuffer 1280x800x32  pitch 5120
text grid 160x50 cells (8x16 font)
```

* `pitch = 宽 × 色深/8`，反映"一行占多少字节"，是画像素时真正的跨距
* `160x50` 是算出来的：1280/8 = 160 列，800/16 = 50 行

这一步的意义：**同一份内核代码，换分辨率后行列自动变**。以前写死 80×25 的地方
（比如 `parse_vga.py` 那种读 `0xB8000` 的思路）在图形模式下全部失效。

#### 7) `res`：运行时改分辨率

长模式下已经调不到 `int 10h`（BIOS 中断只在实模式有），GRUB 也只在启动那一刻定模式。
运行中要换，只能直接跟显卡说话。QEMU / Bochs 的 VGA 提供一套 Bochs dispi 寄存器：

| 端口 | 作用 |
| --- | --- |
| `0x1CE` | 索引 |
| `0x1CF` | 数据 |

```
0x0 ID      读回来必须是 0xB0C0..0xB0C5，否则不是这套接口，直接失败
0x1 XRES    宽
0x2 YRES    高
0x3 BPP     色深
0x4 ENABLE  0x01 开启 | 0x40 用线性帧缓冲 -> 写 0x41
```

顺序是先 `ENABLE=0` 关掉，再写三个参数，最后 `ENABLE=0x41` 开回来。改完立刻按新宽度
重算 `pitch`、列、行，清屏，然后把几何重报一遍。

```
[zh@zh_OS ~]$ res 1024x768
framebuffer 1024x768x32  pitch 4096
text grid 128x48 cells (8x16 font)
[zh@zh_OS ~]$ res 800x600 24
framebuffer 800x600x24  pitch 2400
text grid 100x37 cells (8x16 font)
```

> **分隔符别敲错**：命令里宽高之间用**字母 `x`**（`res 1024x768`），`*` 也认（`res 1024*768`）。
> 文档正文里写的 `1280×800` 那个 `×` 只是排版用的乘号，键盘上打不出来、命令也不认，
> 照着正文敲 `×` 会得到 `res: expected <width>x<height>  (x or *)`。

**真机上会怎样**：读不到 `0xB0Cx` 就直接红字 `res: the video device refused that mode`，
不写端口。真机换模式要靠 VBE BIOS 调用或者 GPU 自己的驱动，那是一条更长的路
（Linux 走的是 `drm` 那一套），这一步先把"内核能按任意分辨率排版"这件事做通。

**换模式不再清屏**（M26 之后）：内核多了整整一层字符缓冲，`res` 会把旧屏幕的
最后几行搬到新屏幕底部、按新几何重绘，见下一节。早期版本是直接清屏的。
Linux 的 fbcon 换模式后能重绘，是因为它留着 scrollback（`vc` 层），我们还没有。

#### 8) 验收

```
bash build/qemu_verify_m25.sh
```

一个 QEMU 实例，登录后依次：

| 屏 | 动作 | screendump 尺寸 | 屏上文字 |
| --- | --- | --- | --- |
| 1 | `videomode`（GRUB auto 给的档） | 1280×800 | `framebuffer 1280x800x32  pitch 5120` / `160x50` |
| 2 | `res 1024x768` | 1024×768 | `pitch 4096` / `128x48` |
| 3 | `res 800x600` | 800×600 | `pitch 3200`（32bpp）/ `100x37` |
| 4 | 再 `videomode` 复核 | 800×600 | 仍是 800×600 |

判据不是"看起来变了"，而是**PPM 的宽高**：`screendump` 出来多大，就说明显卡真的换了模式。

#### 9) 两个内核：文本模式留一条退路

这里有个坑必须写清楚：**GRUB 只要在 Multiboot2 头里看到帧缓冲请求标签（type = 5），
就一定会给图形模式**，连 `set gfxpayload=text` 也压不住。也就是说，加了标签之后，
内核就再也回不到 `0xB8000` 那套文本模式了 —— 而 M2..M24 的所有验收脚本都是读
`0xB8000` 抓屏（`build/parse_vga.py`），一进图形模式全变成空白屏。

解决办法：**同一份源码编出两个内核**，只差 boot 那一个 `.o`。

| 产物 | 编法 | GRUB 给什么 | 谁在用 |
| --- | --- | --- | --- |
| `build/zh_OS.elf` | `nasm -f elf64 boot/boot.asm` | 文本模式 80×25 | 默认菜单项，M2..M24 的验收 |
| `build/zh_OS_fb.elf` | `nasm -f elf64 -DFB_REQUEST boot/boot.asm` | 图形模式 + fb 标签 | 菜单第 2 项，M25 的验收 |

`boot.asm` 里只加了一层条件汇编：

```asm
%ifdef FB_REQUEST
align 8
    dw 5 / dw 0 / dd 20 / dd 0 / dd 0 / dd 0
%endif
```

`build.sh` 的 `link_kernel()` 会先把 `boot_fb.o` 单独编出来，再把 `OBJECTS` 里的
`build/boot/boot.o` 换成它、其余目标文件原封不动地链第二遍。两个 ELF 一起放进 ISO，
菜单两项各指一个。抗风险的设计：**默认项是文本模式**，所以以前那套东西照旧能跑；
要图形模式按一下方向键下再回车。

（顺带一个时序细节：`set timeout=3`。老的验收脚本是 `sleep 4` 之后开始发 `sendkey`，
而 GRUB 菜单**收到任何按键都会停住倒计时**，所以超时时间不能拖到 4 秒以后，
否则脚本的第一下按键就把菜单按停了，永远进不去系统。）

#### 11) 整屏字符缓冲：换分辨率不再丢屏（M26）

M25 的第一版是"算出像素就直接往帧缓冲写"。这在换模式时会露馅：模式一变，旧显存的
内容全部作废，能写的地方只剩新的一块，于是 `res` 只能清屏 —— 屏幕上说过的话全丢了。

真实的内核不是这样。Linux 的 `fbcon` 下面还有一层 `vc`（virtual console），存的是
**字符**，不是像素；模式一变，`vc` 原样留着，`fbcon` 按新的行列重新算一遍坐标再画一遍。
M26 补的就是这一层。

**数据结构**（`drivers/fb.c`）：

```c
#define FB_BUF_COLS 320u        /* 2560x1600 的 8x16 网格正好这么密 */
#define FB_BUF_ROWS 100u

struct fb_cell {                /* 4 字节/格，100*320*4 = 128KB，放 .bss */
    uint8_t ch;                 /* 字符（ = 空白格） */
    uint8_t fg, bg;             /* 存调色板号 0..15，不存 RGB：省空间，换色也不会失真 */
    uint8_t pad;
};
static struct fb_cell g_buf[FB_BUF_ROWS][FB_BUF_COLS];
static uint32_t g_used_rows;    /* 真正用到过几行（= 光标行 + 1） */
```

**四个动作**：

| 函数 | 作用 |
| --- | --- |
| `buf_put()` | 记账：往缓冲的一个格子里写 字符 + 前后景 |
| `draw_cell()` | 落像素：按缓冲里那一格画 8×16 点阵（**全系统唯一真正画字的地方**） |
| `fb_redraw()` | 整屏按缓冲重画（换模式后靠它把内容接回来） |
| `buf_clear()` | 清缓冲（`fb_clear` 里和显存一起清） |

`fb_putchar` 现在只剩"改光标 + `buf_put` + `draw_cell`"，滚屏也同步把缓冲整体上移一行、
末行补空格 —— 缓冲和显存永远是一致的，不会出现"字画了但缓冲里没有"。

**换模式时怎么接回来**（`fb_set_mode` 末尾）：

```c
keep = min(old_used_rows, 新行数, FB_BUF_ROWS);   /* 只搬真正用过的行 */
旧行 [old_used - keep, old_used)  ->  新行 [新行数 - keep, 新行数)
fb_redraw();
g_cy = 新行数 - 1;                                /* 光标贴底，接着往下滚 */
```

两个细节是有意这么定的：

1. **底部对齐，不是顶部对齐**。历史留在屏幕下方、光标跟着贴到底部，
   这样"越新的内容越靠下"这条终端直觉不会被打断；屏幕变高时多出来的空间留在上方。
2. **只搬 used_rows**。要是按"整个旧屏幕"搬，一堆空白行也会被搬过去 ——
   结果历史被顶到最上面、光标在屏幕最底下，中间留一大条空白（第一版就是这么错的，
   截图一眼就能看出来）。

**实测**（`bash build/qemu_verify_m25.sh`，脚本里已经去掉了 `clear`）：

```
1280x800  videomode                       <- 登录横幅 + 第一条 videomode
1280x800  res 1024x768                    <- 换档：旧内容整屏保留，底部对齐
1024x768  videomode -> pitch 4096 / 128x48   <- 新输出接在历史下面
800x600   res 800x600                     <- 再换一次，前面的历史仍然在
```

截图 `build/m25_verify/m25_2_1024.png` 里能同时看到 1280x800 时打的 `pitch 5120 / 160x50`
和换档后打的 `pitch 4096 / 128x48` —— 这就是"M26 之前根本做不到"的那件事。

**还没做的**：缓冲只有"可见的一屏"，没有滚出去的 scrollback（向上翻页要另存环形缓冲，
像 Linux 的 `vc_scrollback`）；也没有多个虚拟终端（Ctrl+Alt+F1/F2 切换）。

#### 12) 已知的坑

1. **GRUB 的 `gfxpayload` 和帧缓冲标签会打架**。标签里写死宽高，菜单里的
   `set gfxpayload=...` 就完全不起作用；标签写 0 又会让 GRUB 一律用"auto"（QEMU 下是
   1280×800）。所以"启动时选分辨率"这条路先放弃，改成 kernel 里用 dispi 换——
   这也更接近真实内核的行为（模式由驱动设，不由 bootloader 定死）。注意：只要标签在，
   `gfxpayload=text` 就压不住，所以才要编两个内核（见上一节）。
2. **QEMU 的显卡被 GRUB 认成 Cirrus 时只能出 800×600×24**。`insmod all_video` 会先加载
   Cirrus 驱动，它的模式表很窄；改用 `insmod vbe` 才能拿到大分辨率。
3. **换模式瞬间显示管线还在按旧跨距扫**，立刻清屏会留一条上一屏的残影。先空转几十毫秒
   （`pause` 8M 次），再 `ENABLE` 关一次开一次逼它整屏重扫，最后连清两遍。
4. **`outw` 的内联汇编约束要用 `"d"(port)`**。用 `"Nd"` 时 GCC 可能给出内存操作数，
   `Error: operand type mismatch for 'out'`。

> 另外还有一个**只在 QEMU 截图里看得到**的现象：换模式后立刻 `screendump`，最上面会留
> 一条上一屏被裁掉一半的旧提示行（约 8 个像素的竖向上移）。内核报的几何数值、以及 PPM
> 的宽高都是对的，属于 QEMU 在 mode switch 瞬间对显示层的复制残留；真机上看不到。
> 留在这里是为了以后别人截图时不会以为内核算错了行列。
### 1920×1080 panic：帧缓冲没映射到那么远 M27（已修复）

这是 `res` 上线后第一个真机反馈：VirtualBox 里敲 `res 1920X1080`，直接

```
KERNEL PANIC: Page Fault (#PF)
error code = 0x0000000000000002
cr2        = 0x00000000E0201C00
rip        = 0x000000000010B7A8
```

#### 1) 先把 error code 读对

`0x2` 拆开看：bit0 `P=0`（页不在）、bit1 `W/R=1`（写）、bit2 `U/S=0`（内核态）。
也就是**内核写了一个没映射的地址**。`cr2` 是被访问的线性地址：`0xE0201C00`。

对比一下：VirtualBox 的 VBE LFB 基址通常在 `0xE0000000`，QEMU 是 `0xFD000000`。
`cr2 - 0xE0000000 = 0x201C00` ≈ **2.1MB 偏移**——正好落在"已经画了一部分、然后越界"的位置。
`rip` 用 `bash build/sym.sh 0x10B7A8` 查出来在 `fb.c` 的写像素路径里。

#### 2) 根因：页表是按启动那一档建的

`fb_init()` 里只映射 GRUB 给的那一档：

```c
paging_map_2mb_range((uint64_t)g_phys, (uint64_t)g_pitch * g_h);
```

1280×800×32 → `5120 * 800 = 4,096,000` 字节，向上取整到 2MB 大页就是 **6MB**。

而 `res 1920X1080` 后需要 `1920*4*1080 = 8,294,400` 字节 ≈ **8.3MB**，
也就是 2MB 大页要 5~6 张才够。`fb_set_mode()` 里只改了 `g_w/g_h/g_pitch` 就直接 `fb_redraw()`，
写到第 6MB 之后就 #PF——**这不是显卡不支持，是内核自己没给那段显存建页表**。

顺带说明为什么 VirtualBox 崩而 QEMU 没被发现：QEMU 也崩（我按同样流程复现到了
一模一样的 `error code=0x2 / rip=0x10B7D8`），只是之前的验收只跑了 1024×768 和 800×600，
那两档都比 6MB 小，永远踩不到这条线。

#### 3) 修法：换模式之前先把新尺寸映射好

`drivers/fb.c` 的 `fb_set_mode()`，在写 dispi 寄存器**之前**加一段：

```c
/* [ADD] M27：先把新尺寸要用的帧缓冲映射出来，再切模式。 */
if (!paging_map_2mb_range((uint64_t)g_phys,
                          (uint64_t)w * (uint64_t)(bpp / 8u) * (uint64_t)h)) {
    return 0;   /* 映射不出来就别切，宁可红字报错也不 panic */
}
```

两个关键点：

- **先映射后切模式**。顺序反了没意义：模式切完显示端就开始按新跨距扫，
  只要新范围里有一段没映射，第一次 `fill_all` 就死。
- **可以反复调用**。`paging_map_2mb_range()` 对已经存在的 PDPT/PD 是复用的，
  已经映射过的 2MB 页只是把同一项重写一遍，不会重复吃 `pd_pool`。
  所以 `1600×900 → 1920×1080 → 800×600 → 1280×800` 这样来回换都安全。

失败时返回 0，`cmd_res` 会打红字 `res: the video device refused that mode`，内核继续跑。

#### 4) 顺手修掉第二个毛病：换档后历史被压到屏幕最底下

修完 panic 第一次跑 1920×1080，屏是活的，但很难看：16 行历史被"底部对齐"搬到 67 行屏幕的最后，
中间空出 40 多行黑屏，光标吊在最底下。M26 的底部对齐是按"历史总是快满屏"设计的，
1080p 下不成立。

改成按**装不装得下**决定：

```c
keep = old_used < g_rows ? old_used : g_rows;
/* dst_row = i2 —— 历史贴顶部 */
...
g_used_rows = (keep < g_rows) ? keep : g_rows;
g_cy        = (keep < g_rows) ? keep : (g_rows - 1u);
```

- 历史够多（`keep == g_rows`）：搬最后 `g_rows` 行，屏满，和 M26 完全一致，1024→800 的验收不变；
- 历史装得下：贴顶部，光标紧跟历史最后一行，下面留白——这才像终端。

顺带把新屏"多出来的列"和"历史下面的空行"一起刷成空格，否则右侧会残留旧屏的像素。

#### 5) 验收

`build/_hd.sh`（QEMU `-vga std`，图形菜单项，登录后 `res 1920*1080`）：

```
m27_fix2 -> 1920 1080
```

截图 `build/m25_verify/m27_fix2.png` 上：

```
[zh@zh_OS ~]$ videomode
framebuffer 1280x800x32  pitch 5120
text grid   160x50 cells (8x16 font)
[zh@zh_OS ~]$ res 1920*1080
framebuffer 1920x1080x32  pitch 7680
text grid   240x67 cells (8x16 font)
```

没有 panic，历史完整留在屏幕上，提示符紧跟其后。修复前的对比图是
`build/m25_verify/m27_1080x.png`（同一处 #PF），留档用。

> 注意：QEMU 的 `sendkey` 送不出大写 `X`（键名表里没有），所以脚本里一律用 `res 1920*1080`。
> 真键盘上 `1920X1080`、`1920x1080`、`1920*1080` 三种都能过，解析器这三种分隔符都收。

#### 6) 这个坑的通用教训

**凡是"运行时变大"的资源，页表都要跟着长**：帧缓冲、用户栈、`brk` 堆、mmap 区域。
内核里没有缺页中断兜底的时候，唯一正确的做法就是"先建映射、再动指针"。
`fb_set_mode()` 现在就是这个顺序，`paging_map_2mb_range()` 也已经是幂等的。

## 八、实现说明（摘要）

- **引导**：Multiboot2 要求 bootloader 以 32 位保护模式跳入内核。`boot/boot.asm` 先建
  PML4/PDPT/PD（512 个 2MB 大页映射低 1GB）、开 PAE/LME/PG，再远跳进 64 位；
  进入内核后由 `gdt.c` / `paging.c` 重建 GDT 与页表。
- **中断**：`interrupt.asm` 的 48 个 stub 统一栈帧（错误码占位 + 中断号 + 15 个通用寄存器，
  共 176 字节），公共入口显式 `and rsp, ~0xF` 保证 SysV ABI 对齐；`isr.c` 分发异常与 IRQ，
  `pic.c` 把 IRQ0-15 重映射到向量 32-47；`keyboard.c` 中断侧只入 256 字节环形缓冲。
- **行输入**：`console.c` 的 `read_line()` / `read_password()` **共用一块 128 字节静态缓冲**，
  连续两次调用必须读到就拷贝（`login.c`、`shell.c`、`commands.c` 都遵守）。
  也因此彩色提示符由 Shell 自绘、再传空提示 `read_line("")`。
- **账户**：`users.c` 只存 FNV-1a 64 位哈希，`user_auth()` 无论账户是否存在都先算一次哈希；
  `user_add()` 仅 root，`user_set_password()` 普通用户只能改自己。
  `users_persist.c` 在不动既有账户逻辑的前提下，通过 `user_table_entry()` /
  `user_table_reset_and_load()` 两个只读桥接函数完成整表导出与整表覆盖加载。
- **ramfs**：`fs/ramfs.c` 用 `kmalloc` 建 inode 树（名长 32、最多 32 个子女），
  路径解析支持绝对/相对、`.`/`..`，最多 8 段；写操作前检查
  「root 全树 / 普通用户限 `/home/<name>` 子树」，越权返回 `RAMFS_ERR_PERM`。
  当前目录由 `shell.c` 的 `shell_cwd_fn_t` 钩子取到提示符里打印。
- **ATA**：`drivers/ata.c` 纯 PIO、轮询式，`ata_init()` 用 IDENTIFY（0xEC）探测主盘并缓存
  存在标志；读写均按 LBA28 组装 `0x1F2..0x1F6`，400ns 延时靠连读四次 `0x3F6`。
  16 位端口读写必须用内联汇编的 `%w1` 修饰符（`"d"` 约束会生成 `%edx`，汇编器拒绝 `out/in`）。
- **FAT16**：`fs/fat16.c` 先解析 BPB 求出 FAT 表 / 根目录 / 数据区起始 LBA，
  再按 8.3 目录项 + FAT 簇链（≥`0xFFF8` 为链尾）遍历；名字匹配大小写不敏感，
  列举时跳过卷标、LFN 与 `.`/`..`；全程只读，不写任何扇区。
- **Shell**：`shell.c` 主循环读行 → 立即拷贝 → 切分 argv[16] → 查静态命令表；
  `logout` 返回 `SHELL_LOGOUT` 让 `kernel_main` 重新进入登录流程。
  各命令实现分散在 `commands.c`（基础命令）与 `cmd_ramfs.c` / `cmd_disk.c` /
  `cmd_fat.c` / `cmd_time.c`（后续增量功能），命令表统一在 `commands.c` 末尾追加。
- **会话循环**：`kernel_main` 末尾为 `for (;;) { login_screen(); shell_run(); }`，
  进入循环之前完成 `ata_init()` + `users_load()` 与 `[ATA]` 日志。

## 九、文件职责速查

| 文件 | 职责 |
| --- | --- |
| `kernel/version.h` | 统一版本号宏（banner / uname / 登录横幅 / GRUB 菜单） |
| `boot/boot.asm` | Multiboot2 头、32->64 位入口、临时页表、16KB 栈 |
| `kernel/gdt.c` `paging.c` | 64 位 GDT、4 级页表与 CR3 |
| `kernel/idt.c` `interrupt.asm` `isr.c` `pic.c` | 中断门、stub、分发、8259A 重映射 |
| `kernel/console.c` | 行编辑器（共享静态缓冲） |
| `drivers/vga.c` `keyboard.c` | 彩色文本输出、PS/2 键盘 IRQ1 |
| `drivers/pit.c` | PIT 100Hz 计数器（uptime / date / 日志 tick） |
| `drivers/ata.c` `ata.h` | ATA PIO 读写扇区、主/从盘探测、`diskinfo` |
| `users/users.c` `login.c` | 内存态账户表、FNV-1a 哈希、登录界面与 3 次失败重来 |
| `users/users_persist.c` | 账户表序列化到 LBA 200-207（8 扇区 / 4KB） |
| `shell/shell.c` `commands.c` | 彩色提示符、统一输出宏、45 条命令表 |
| `shell/cmd_ramfs.c` | `ls/cd/pwd/cat/mkdir/touch/rm`（ramfs） |
| `shell/cmd_disk.c` | `diskinfo`（ATA 磁盘与持久化扇区） |
| `shell/cmd_fat.c` | `fatmount/fatls/fatcat`（FAT16 只读） |
| `fs/ramfs.c` `ramfs.h` | 内存文件系统：inode 树、路径解析、home 子树权限 |
| `fs/fat16.c` `fat16.h` | FAT16 只读：BPB 解析、8.3 名、簇链跟随 |
| `mm/heap.c` `heap.h` | 2MB 静态池 first-fit 堆（合并、越界/重复 free 报错） |
| `arch/cpuinfo.c` | CPUID 采集（vendor / brand / family-model-stepping / 特性） |
| `kernel/klog.c` `history.c` | 内核日志环（128 槽）、Shell 历史召回 |
| `kernel/task.c` `task.h` | M4 进程与调度：TCB、引导帧、时钟抢占、环形轮转、异常杀任务 |
| `kernel/task_switch.asm` | M4 `zh_sched_enter` / `zh_sched_leave` / `zh_resume_frame`（按中断帧换人） |
| `kernel/vmm.c` `vmm.h` | M5 进程地址空间：每进程 PML4/PDPT/PD/PT、4KB 页映射、CR3 切换、M7 整页克隆（fork 用） |
| `kernel/elf.c` `elf.h` | M6 ELF64 装载器：头校验、PT_LOAD 逐段映射拷贝、.bss 清零、取 e_entry |
| `kernel/tss.c` `tss.h` | M1 7 项 GDT + 64 位 TSS（RSP0 每任务重写） |
| `kernel/ring3.asm` `ring3.h` | M2 进 ring3 保存内核现场 / `usermode_resume` 回内核 |
| `kernel/usermode.c` | M1 ring3 自检：拼机器码、页表 U/S 置位、iretq 降权 |
| `kernel/syscall.c` `syscall.h` | M2 `int 0x80`：write/ticks/exit/cpl（M4 加 putc/sleep） |
| `mm/phys.c` `phys.h` | M3 Multiboot2 内存图 + 4KB 页帧位图分配器 |
| `shell/cmd_user.c` `cmd_syscall.c` `cmd_memmap.c` `cmd_task.c` `cmd_vmm.c` `cmd_elf.c` | `ring3` / `syscall` / `memmap` / `tasks,taskrun` / `vmspace` / `elf` |
| `kernel/kernel.c` | 初始化各子系统，最后进入登录 + Shell 循环 |
| `build.sh` / `setup.sh` / `check.sh` | 构建 ISO / 配置环境 / 环境体检 |
| `linker.ld` | 段布局与 0x100000 加载地址 |

所有源码文件顶部都有标准文件头注释（文件名、版本 `0.2beta`、功能简述）。
