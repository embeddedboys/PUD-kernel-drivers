# 构建

> Makefile **不硬编码任何内核路径**，全部命令行传入；同一份源码支持厂商 6.1 源码树、板子运行内核的
> headers、以及本机 generic 内核三种模式，靠 `vermagic` 必须与目标内核一致来约束。

## TL;DR

- 变量：`KERN_DIR` / `KERN_OBJ_DIR` / `ARCH` / `CROSS_COMPILE`；默认取本机运行内核。
- 内核用 `O=` 分离构建时必须传 `KERN_OBJ_DIR`（源码树里没有 linker 需要的生成文件）。
- **绝不修改 `KERN_DIR` 指向的目录**；需要 objtree 就传 `KERN_OBJ_DIR`，不要在源码树里补文件。
- 构建开关 `PUD_USB_ASYNC=0|1` 选 EP1 传输路径；一个镜像只编一条。
- 验证 `vermagic`：`modinfo pud.ko | grep vermagic` 必须等于目标内核 `uname -r`。
- 真机/QEMU 验证流程见 [board-testing.md](board-testing.md)。

## Makefile 变量

| 变量 | 默认 | 含义 |
| --- | --- | --- |
| `KERN_DIR` | `/lib/modules/$(uname -r)/build` | 目标内核源码树 |
| `KERN_OBJ_DIR` | 空 | 该内核的 **out-of-tree 构建目录（objtree）**，仅当内核用 `O=` 编译时需要 |
| `ARCH` | 本机（`uname -m`） | 目标架构。交叉编译要显式传 `ARCH=arm64` |
| `CROSS_COMPILE` | 空（本机工具链） | 交叉编译器前缀，例如 `aarch64-linux-gnu-` |

```bash
make modules                                       # 本机运行内核（默认全对）
make modules ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make modules KERN_DIR=<...> [KERN_OBJ_DIR=<...>]
make clean   KERN_DIR=<...> [KERN_OBJ_DIR=<...>]
```

**为什么需要 `KERN_OBJ_DIR`**：内核用 `O=` 分离构建时，链接模块所需的生成文件（`scripts/module.lds`、
`Module.symvers`、`.config`）都在 objtree 里，源码树里没有。不传会出现 `scripts/module.lds: No such file`。

## 构建选项：`PUD_USB_ASYNC`

| 值 | EP1 传输路径 |
| --- | --- |
| `0`（默认） | 同步 `usb_sg`（`usb_sg_init`/`usb_sg_wait`，栈上 timer → `usb_sg_cancel`） |
| `1` | 异步 URB（`usb_submit_urb` + completion，`wait_for_completion_timeout` → `usb_kill_urb`） |

```bash
make modules KERN_DIR=<...> PUD_USB_ASYNC=1
```

两条路径状态不共享，一个镜像只编一条。异步路径的 DMA 源仍是 `pud->encoder_buf`
（`dma_alloc_coherent`），URB 直接带 `URB_NO_TRANSFER_DMA_MAP` + `transfer_dma = pud->encoder_dma`，
**不**让 USB 核心去 map vmap 地址 —— 这正是同步路径要建 SG 表的原因（见 [pitfalls.md](pitfalls.md) 1.2）。
两条路径的成功返回值相同（payload 字节数），调用方（`pud_flush`、DRM commit）不变。

### 真机对比结论（2026-09，同步 vs 异步）

环境：RK3588 系 xHCI（`fc400000.usb`）+ RP2350 固件。两个自然失败场景下两条路径**表现一致**，都没有
把板子弄卡：

| 场景 | 同步 `usb_sg` | 异步 URB |
| --- | --- | --- |
| 设备停摆（SWD halt 住 Pico）后触发 flush | `-ETIMEDOUT`（约 3 s）后返回；`rmmod` 5183 ms 成功 | 同样 `-ETIMEDOUT`；`rmmod` 5247 ms 成功 |
| 压屏中断开重枚举（复位 Pico） | 干净重新 probe，板子保持响应 | 同样 |

> **历史纪律（针对老固件）**：EP1 stall → 主机 `clear_halt` 重试这条路径曾把宿主控制器卡死到连板子都
> 重启不干净，所以 `pud_flush()` 里那段 `usb_clear_halt()` 现在只是防御。
> **当前固件已不再 stall**：对不可信 header（`12+size` 超限、矩形越界）是**丢弃并重新武装**
> （`g_ep1_stat.oversize` 增长，宿主看不到错误；用临时超限补丁验证），所以**没能在当前硬件上复现出
> 同步路径卡死**。这条纪律保留，因为老固件与控制器级 stall 仍可能发生。

## 三种构建模式

### A. 厂商内核源码树（6.1.172，objtree 模式）

```bash
make modules \
  KERN_DIR=<vendor-kernel-src> \
  KERN_OBJ_DIR=<vendor-kernel-objtree>
```

例如 `KERN_DIR=<...>/kernel-6.1`、`KERN_OBJ_DIR=<...>/build/linux-rockchip`
（厂商内核用 `O=` 分离构建，源码树与 objtree 是两棵目录）。

> ⚠️ **绝不修改 `KERN_DIR` 里的任何东西**。它是共享的内核源码目录，只作为头文件/符号来源。

### B. 板子运行内核的 headers（6.1.172，headers 模式）

板子跑的内核版本可能与 `KERN_DIR` 不同。模块 `vermagic` 必须与运行内核一致，否则 `insmod` 报：

```
pud: disagrees about version of symbol module_layout
```

**在板子上直接编**（版本天然一致，`KERN_DIR` 默认就对）：`make modules`。

**在 x86-64 开发机上交叉编**：

```bash
# 1) 从板子取 headers（板子上有 /usr/src/linux-headers-$(uname -r)）
tar czf hdrs-$(uname -r).tgz -C /usr/src linux-headers-$(uname -r)

# 2) 在 x86-64 开发机上解出，目录形如
#    .pud-test/linux-headers-6.1.172/{Makefile,Module.symvers,arch,include,scripts}

# 3) 用该 headers 树编模块
make -C .pud-test/linux-headers-6.1.172 \
     M=$PWD ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

编完确认 `vermagic`：

```bash
modinfo pud.ko | grep vermagic
# vermagic:  6.1.172 SMP mod_unload modversions aarch64
```

### headers 里的 host 工具是 x86-64（板子上本地编的坑，Makefile 已处理）

板子 `/usr/src/linux-headers-6.1.172/scripts/basic/fixdep` 是 **x86-64** 的（厂商在 x86-64 主机上交叉
编译出 arm64 内核，`fixdep`/`modpost` 这类 host 工具跟着编成了 x86-64；拷到 WSL 后 BuildID 与板子上
完全一致，所以交叉编能直接跑）。在 arm64 板上执行就是：

```
/bin/sh: 1: scripts/basic/fixdep: Exec format error
```

这个坑没法让 kbuild 自己修：目录 root 只读；headers 包里没有任何 Kconfig，而
`include/config/auto.conf.cmd` 把一大串不存在的 Kconfig 列成依赖，所以 `make scripts_basic` /
`make modules_prepare` 会先去跑 `syncconfig` 然后失败；把 `fixdep` 删掉也不会被重建 —— 外模块路径
（`make M=... modules`）根本不构建 host 工具，只会得到 `scripts/basic/fixdep: not found`。

所以 `Makefile` 在**检测到 host 工具不是本机架构**（读 ELF 头 offset 18 的 `e_machine`，只用 `od`）时会：

1. 把 headers 树拷到 `~/.cache/pud-kbuild/<kernel release>`，按版本缓存。拷贝用 `realpath` 解析后的路径：
   `/lib/modules/$(uname -r)/build` 是符号链接，`cp -a` 会把链接本身拷过去，编译就写进只读的原目录
   （`Permission denied`）；
2. 在副本里按 kbuild 自己记在 `.cmd` 里的命令，用**本机 gcc** 重建 `fixdep`/`modpost`
   （含 `mk_elfconfig`、`elfconfig.h`），flags 保持内核原样；
3. 之后的构建全部用这个副本，**`KERN_DIR` 始终只读**。

实测：板子上从零 `make` 到 `pud.ko` 全绿，`vermagic` 正确；副本 65 MB，删掉
`~/.cache/pud-kbuild/<release>` 即重做。x86 上交叉编不受影响。

### C. 本机内核（x86-64）

本分支编的就是**本机运行内核**，`KERN_DIR`、`ARCH`、`CROSS_COMPILE` 默认全对：

```bash
make modules
```

想在不冒"把本机内核搞崩"的风险下验证驱动，用 `make qemu`（下一步见 [board-testing.md](board-testing.md)）。

## 其它环境注意事项

- 交叉编译器：`aarch64-linux-gnu-gcc`（Debian/Ubuntu 包 `gcc-aarch64-linux-gnu`）。
- 若 `rmmod` 后立刻 `insmod` 报 `File exists`，说明上一次卸载没干净 —— 重启板子。
- 编译产物（`*.o`、`*.ko`、`*.mod*`、`build/`、`compile_commands.json`）已在 `.gitignore` 中，不要提交。
- `make modules` 会顺带生成 **`compile_commands.json`**（用内核的
  `scripts/clang-tools/gen_compile_commands.py` 解析 kbuild 的 `.*.cmd`），仓库根 `.clangd`
  指向它；交叉编译器头文件路径报缺失时用 `clangd --query-driver=/usr/bin/aarch64-linux-gnu-*` 启动。
