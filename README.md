# zh_OS 0.1beta (x86_64)

zh_OS 是一个教学用的 x86_64 64 位微型内核：GRUB2 + Multiboot2 引导，NASM + freestanding C
（无 libc），运行在 80x25 VGA 彩色文本模式下。它已经具备长模式、4 级页表、GDT、IDT/ISR/PIC、
PS/2 键盘、行编辑器、仿 Linux 的账户登录、以及带彩色提示符的内置 Shell。

- **统一版本号**：`zh_OS 0.1beta (x86_64)`。启动横幅、`uname` 命令、登录横幅与 GRUB 菜单项
  都由 `kernel/version.h` 的 `ZH_OS_VERSION_TEXT` 提供；`build.sh` 在构建时会校验脚本侧的
  `VERSION_TEXT` 与它一致，防止只改一处造成版本漂移。
- **开发/构建环境**：宿主机 Windows 11，构建环境 WSL2 + Ubuntu。所有脚本都是 Linux `.sh`，
  在 WSL 内运行，**不生成任何 `.bat`**。
- **可启动产物**：`zh_OS-0.1beta.iso`（Hybrid ISO，可用于 QEMU / VirtualBox / VMware）。

> **首次登录**：内置 `root`（uid 0 / gid 0 / `/root` / 管理员）与
> `zh`（uid 1000 / gid 1000 / `/home/zh`），**两者初始密码都是 `zh`**。
> `useradd` 新建的账户初始密码同样是 `zh`。账户表全内存态，重启即复位。

## 目录结构

```
zh_OS/
├── boot/           引导代码（Multiboot2 头、32->64 位两段式入口、临时页表）
├── kernel/         内核核心（GDT、页表、IDT、中断分发、PIC、控制台、主入口、版本头）
│   └── version.h   全项目唯一的版本号宏（ZH_OS_VERSION_TEXT）
├── drivers/        设备驱动（vga：80x25 彩色文本；keyboard：PS/2 IRQ1 环形缓冲）
├── users/          账户管理与登录（users.c/.h、login.c/.h；含 ATA 持久化 TODO）
├── shell/          Shell（shell.c/.h、commands.c）——彩色提示符与 18 个内置命令
├── mm/             内存管理（本版未实现，仅 .gitkeep 占位）
├── docs/           历史截图（第 2 步的彩色横幅、调色板自检）
├── build/          中间产物：.o、内核 ELF、QEMU 屏取与可复用验证脚本
├── iso/            ISO 组织目录（boot/zh_OS.elf、boot/grub/grub.cfg）
├── linker.ld       链接脚本（加载地址 0x100000，.bss 4K 对齐）
├── setup.sh        环境配置（安装依赖、可选交叉工具链、写 env.mode）
├── check.sh        只读环境体检：打印 gcc/nasm/grub-mkrescue/xorriso/qemu 版本
├── build.sh        一键编译 + grub-mkrescue 出 ISO
├── env.mode        工具链路线（system-gcc / cross）
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
| `zh_OS-0.1beta.iso` | 可启动 ISO，供 QEMU / VirtualBox / VMware 使用 |
| `build/zh_OS.elf` | 内核 ELF（调试用） |
| `iso/` | ISO 组织目录（`iso/boot/zh_OS.elf`、`iso/boot/grub/grub.cfg`） |

`build.sh` 读取 `env.mode` 决定工具链（`system-gcc` 用系统 gcc/ld，`cross` 用
`toolchain/bin/x86_64-elf-gcc`）。它会自动扫描
`kernel/ boot/ drivers/ mm/ shell/ users/` **顶层**的所有 `.c` / `.asm`，
头文件搜索路径为 `-I kernel -I drivers -I users -I shell`，
**不需要手工维护编译列表**。

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

1. GRUB 菜单显示 **zh_OS 0.1beta (x86_64)**，自动进入内核。
2. 短暂显示 Linux printk 风格启动日志（banner、`gdt:` / `paging:` / `vga:` / `idt:` /
   `pic:` / `keyboard:` / `users:`，亮绿 `Long mode active.` 与 `Interrupts enabled (sti).`）。
3. `login_screen()` 清屏，画蓝框 + 三色标题 `zh_OS 0.1beta (x86_64) Login`。
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
| `uname` | 亮绿输出 `zh_OS 0.1beta (x86_64)` | 全部用户 |
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
qemu-system-x86_64 -cdrom zh_OS-0.1beta.iso -m 128
```

关闭 QEMU：按 `Ctrl+Alt+G` 释放鼠标后关闭窗口，或在终端按 `Ctrl+C`。

无头回归验证（无需 X，用 HMP monitor 注入按键并截屏）可复用 `build/` 下的脚本：

```bash
bash build/qemu_verify_step5.sh     # 登录 -> help -> color -> useradd -> su root -> logout -> test
bash build/qemu_verify_extra.sh     # 未知命令 / echo / uname / whoami / passwd / shutdown
bash build/qemu_verify_reboot.sh    # reboot 后重新引导到登录界面
python3 build/check_step5.py        # Windows/WSL 侧静态检查（宏、符号、ASCII、命令表）
```

### 6.2 Windows 下 VirtualBox

先把 ISO 复制到 Windows（在 WSL 内执行，把用户名换成实际的）：

```bash
cp zh_OS-0.1beta.iso /mnt/c/Users/你的用户名/Desktop/
```

然后在 VirtualBox 中：

1. 新建虚拟机 → 类型 **Linux**，版本 **Other / Unknown 64-bit**
2. 内存 **128 MB** 以上，不创建虚拟硬盘
3. 设置 → 存储 → 光驱选择桌面上的 `zh_OS-0.1beta.iso`
4. 启动，GRUB 菜单出现后回车

### 6.3 Windows 下 VMware Workstation

同样先把 ISO 复制到 Windows 桌面（命令同上），然后：

1. 新建虚拟机 → 典型（或自定义）→ 稍后安装操作系统
2. 客户机操作系统选 **Linux**，版本 **其他 Linux 2.6 x86_64**
   （Other Linux 2.6.x kernel 64-bit）
3. 内存 128 MB 以上，光驱（CD/DVD）加载 `zh_OS-0.1beta.iso`
4. 启动虚拟机

> VirtualBox / VMware 都是 Windows 图形程序，需要在宿主机手动操作；
> 本仓库的自动化验证脚本只覆盖 QEMU（无头）。ISO 本身是标准 Hybrid ISO，
> 三种虚拟机加载的是同一个文件。

### 6.4 验收清单（QEMU 已实测通过）

| 操作 | 预期结果 |
| --- | --- |
| `login: zh` / `Password: zh` | 绿色 `Welcome back, zh!` + 绿色 `[zh@zh_OS ~]$` |
| `help` | 彩色 18 条命令列表 |
| `color` | 一屏 16 色板 |
| 普通用户 `useradd test` | 红字 `Permission denied.` |
| `su root` + `Password: zh` | 红色 `[root@zh_OS ~]#` |
| root `useradd test` | 绿色成功提示（uid=1002, default password: zh） |
| `logout` → `login: test` / `zh` | 绿色 `Welcome back, test!` |
| `passwd` 改密后重新登录 | 新密码生效（内存态，重启复位） |
| `shutdown` / `reboot` | 黄字停机 / 重新引导到登录界面 |

## 七、已知限制

- **账户全内存态**：没有文件系统与磁盘驱动，账户表是静态数组；新增账户与改过的密码
  重启后全部丢失、复位为 `zh`。只存 FNV-1a 64 位哈希（**非密码学哈希**，仅教学演示）。
- **无文件系统、无磁盘持久化**：本版不实现可选的 ATA PIO 持久化，接口以 TODO 注释形式
  留在 `users/users.c`（见下节）。
- **内存管理极简**：`boot.asm` 与 `kernel/paging.c` 只用 2MB 大页恒等映射低 1GB，
  没有物理页分配器、堆分配器、缺页处理，也**不解析 Multiboot2 传入的内存地图**
  （magic 与 info 被忽略）。`mm/` 仍是空目录。
- **无 SMP**：单处理器、无 APIC、无多核启动；启动 banner 只写 `#1`，不再写 `SMP`。
- **定时器有限**：已实现 PIT 100Hz（`drivers/pit.c`，IRQ0 自增 `pit_ticks`），
  用于 `uptime` / `date` 与内核日志 tick；但**没有定时调度、没有 APIC timer、没有墙上时钟
  时间戳**（日志只带 tick 计数，不解析 RTC 之外的绝对时间）；登录连续失败 3 次后
  「按回车继续」仍是用来代替延时的。
- **无用户态**：没有 ring 3、系统调用、进程与调度；Shell 与命令全部运行在 ring 0，
  `~` 是写死的路径占位符，没有当前目录概念。
- **键盘只支持扫描码集 1** 的可打印 ASCII、Shift、CapsLock、退格、回车；无组合键、
  无功能键、无 USB 键盘。
- **显示**：仅 VGA 文本模式 80x25 / CP437，无图形模式、无 UTF-8 中文字模。
- **电源管理**：`reboot` 依赖 8042 复位线；`shutdown` 只是 `cli; hlt`，不触发 ACPI 断电。
- **从 ISO 启动是只读的**：CD/ISO 介质无法写入，因此即使实现 ATA 写扇区，
  从 ISO 启动时也没有可写目标，持久化不会生效。

### 可选：ATA PIO 持久化（本版未实现）

没有实现 `drivers/ata.c` 与 `users_load()` / `users_save()`，原因：不改已验收的交互行为、
不引入新依赖，且 ISO 启动场景下写盘没有意义。`users/users.c` 顶部保留了完整的 TODO 接口说明：

- 计划接口：`ata_read_sector(uint32_t lba, uint8_t *buf)` /
  `ata_write_sector(uint32_t lba, const uint8_t *buf)`，LBA28、主通道 `0x1F0`、PIO 模式。
- 计划把账户表序列化为 `name:uid:gid:home:shell:pass_hash:is_admin\n` 文本，
  写入固定扇区（预留 LBA 1），启动时由 `user_init()` 优先 `users_load()`，失败再回退到内置账户。
- **前置条件**：必须给虚拟机挂一块可写磁盘（例如
  `qemu-system-x86_64 -drive file=disk.img,format=raw -cdrom zh_OS-0.1beta.iso -m 128`）。
  从 ISO/CD 启动时扇区写入会失败，只有挂载可写磁盘时持久化才有意义。

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
- **Shell**：`shell.c` 主循环读行 → 立即拷贝 → 切分 argv[16] → 查静态命令表；
  `logout` 返回 `SHELL_LOGOUT` 让 `kernel_main` 重新进入登录流程。
- **会话循环**：`kernel_main` 末尾为 `for (;;) { login_screen(); shell_run(); }`。

## 九、文件职责速查

| 文件 | 职责 |
| --- | --- |
| `kernel/version.h` | 统一版本号宏（banner / uname / 登录横幅 / GRUB 菜单） |
| `boot/boot.asm` | Multiboot2 头、32->64 位入口、临时页表、16KB 栈 |
| `kernel/gdt.c` `paging.c` | 64 位 GDT、4 级页表与 CR3 |
| `kernel/idt.c` `interrupt.asm` `isr.c` `pic.c` | 中断门、stub、分发、8259A 重映射 |
| `kernel/console.c` | 行编辑器（共享静态缓冲） |
| `drivers/vga.c` `keyboard.c` | 彩色文本输出、PS/2 键盘 IRQ1 |
| `users/users.c` `login.c` | 内存态账户表、FNV-1a 哈希、登录界面与 3 次失败重来 |
| `shell/shell.c` `commands.c` | 彩色提示符、统一输出宏、18 个内置命令 |
| `kernel/kernel.c` | 初始化各子系统，最后进入登录 + Shell 循环 |
| `build.sh` / `setup.sh` / `check.sh` | 构建 ISO / 配置环境 / 环境体检 |
| `linker.ld` | 段布局与 0x100000 加载地址 |

所有源码文件顶部都有标准文件头注释（文件名、版本 `0.1beta`、功能简述）。
