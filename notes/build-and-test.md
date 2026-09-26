# 构建与真机验证

## Makefile 设计

`Makefile` **不硬编码任何内核路径**，全部通过命令行变量传入：

| 变量 | 默认 | 含义 |
| --- | --- | --- |
| `KERN_DIR` | `/lib/modules/$(uname -r)/build` | 目标内核源码树 |
| `KERN_OBJ_DIR` | 空 | 该内核的 **out-of-tree 构建目录（objtree）**，仅当内核是用 `O=` 编译时才需要 |
| `ARCH` | `arm64` | 目标架构 |
| `CROSS_COMPILE` | `aarch64-linux-gnu-` | 交叉编译器前缀 |

```bash
make modules   KERN_DIR=<...> [KERN_OBJ_DIR=<...>]
make clean     KERN_DIR=<...> [KERN_OBJ_DIR=<...>]
```

**为什么需要 `KERN_OBJ_DIR`**：内核用 `O=` 分离构建时，
链接模块所需的生成文件（`scripts/module.lds`、`Module.symvers`、`.config`）都在 objtree 里，
源码树里没有。不传就会出现 `scripts/module.lds: No such file` 之类的失败。

## 构建选项

| 变量 | 默认 | 含义 |
| --- | --- | --- |
| `PUD_USB_ASYNC` | `0` | EP1 传输路径。`0` = 同步 `usb_sg`（`usb_sg_init`/`usb_sg_wait`，栈上 timer → `usb_sg_cancel`）；`1` = 异步 URB（`usb_submit_urb` + completion，`wait_for_completion_timeout` → `usb_kill_urb`）。两条路径状态不共享，一个镜像只编一条 |

```bash
make modules KERN_DIR=<...> PUD_USB_ASYNC=1
```

异步路径的 DMA 源仍是 `pud->encoder_buf`（`dma_alloc_coherent`），URB 直接带
`URB_NO_TRANSFER_DMA_MAP` + `transfer_dma = pud->encoder_dma`，**不**让 USB 核心去 map
vmap 地址 —— 这正是同步路径要建 SG 表的原因（见 [pitfalls.md](pitfalls.md) 1.2）。
两条路径的成功返回值相同（payload 字节数），调用方（`pud_flush`、DRM commit）不变。

### 真机对比结论（2026-09）

板子：RK3588 系 xHCI（`fc400000.usb`）+ RP2350 固件。两个自然失败场景下，两条路径
**表现一致**，都没有把板子弄卡：

| 场景 | 同步 `usb_sg` | 异步 URB |
| --- | --- | --- |
| 设备停摆（SWD halt 住 Pico）后触发 flush | `-ETIMEDOUT`（约 3 s）后返回；`rmmod` 5183 ms 成功 | 同样 `-ETIMEDOUT`；`rmmod` 5247 ms 成功 |
| 压屏中断开重枚举（复位 Pico） | 干净重新 probe，板子保持响应 | 同样 |

**没能在当前硬件上复现出同步路径卡死**，因为触发点已经在固件侧消失：固件对一个不可信
header（`12+size` 超限、矩形越界）是**丢弃并重新武装**，不再 stall EP1
（`g_ep1_stat.oversize` 增长，宿主看不到错误；用临时超限补丁验证）。历史上是
"stall → 主机 `clear_halt` 重试"才把宿主控制器卡死到连板子都重启不干净，所以
`pud_flush()` 里那段 `usb_clear_halt()` 现在只是防御（老固件/控制器级 stall）。

## 两种构建模式

### A. 厂商内核源码树（6.1.172，objtree 模式）

```bash
make modules \
  KERN_DIR=<vendor-kernel-src> \
  KERN_OBJ_DIR=<vendor-kernel-objtree>
```

例如 `KERN_DIR=<...>/kernel-6.1`、`KERN_OBJ_DIR=<...>/build/linux-rockchip`
（厂商内核用 `O=` 分离构建，源码树与 objtree 是两棵目录）。

> ⚠️ **绝不修改 `KERN_DIR` 里的任何东西**。它是共享的内核源码目录，
> 只作为头文件/符号来源。所有改动都留在本仓库。

### B. 板子运行内核的 headers（6.1.172，headers 模式）

板子跑的内核版本可能与 `KERN_DIR` 不同。模块的 `vermagic` 必须与运行内核一致，
否则 `insmod` 会报：

```
pud: disagrees about version of symbol module_layout
```

**在板子上直接编**（版本天然一致，`KERN_DIR` 默认就对）：

```bash
make modules            # = /lib/modules/$(uname -r)/build
```

**在 x86-64 开发机上交叉编**（板子在跑别的事情时更方便）：

```bash
# 1) 从板子取 headers（板子上有 /usr/src/linux-headers-$(uname -r)）
tar czf hdrs-$(uname -r).tgz -C /usr/src linux-headers-$(uname -r)

# 2) 在 x86-64 开发机上解出
#    目录形如 .pud-test/linux-headers-6.1.172/{Makefile,Module.symvers,arch,include,scripts}

# 3) 用该 headers 树编模块
make -C .pud-test/linux-headers-6.1.172 \
     M=$PWD ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

编完用 `modinfo` 确认 `vermagic` 与 `uname -r` 一致：

```bash
modinfo pud.ko | grep vermagic
# vermagic:  6.1.172 SMP mod_unload modversions aarch64
```

### headers 里的 host 工具是 x86-64（板子上本地编的坑，Makefile 已处理）

板子 `/usr/src/linux-headers-6.1.172/scripts/basic/fixdep` 是 **x86-64** 的
（厂商在 x86-64 主机上交叉编译出 arm64 内核，`fixdep`/`modpost` 这类 host 工具跟着
编成了 x86-64；拷到 WSL 后 BuildID 与板子上的完全一致，所以交叉编能直接跑）。
在 arm64 板子上执行就是：

```
/bin/sh: 1: scripts/basic/fixdep: Exec format error
```

这个坑没法让 kbuild 自己修：目录是 root 只读的；headers 包里没有任何 Kconfig，而
`include/config/auto.conf.cmd` 把一大串不存在的 Kconfig 列成依赖，所以
`make scripts_basic` / `make modules_prepare` 会先去跑 `syncconfig` 然后失败；
把 `fixdep` 删掉也不会被重建 —— 外模块路径（`make M=... modules`）根本不构建 host 工具，
只会得到 `scripts/basic/fixdep: not found`。

所以 `Makefile` 在**检测到 host 工具不是本机架构**（读 ELF 头 offset 18 的
`e_machine`，只用 `od`）时会：

1. 把 headers 树拷到 `~/.cache/pud-kbuild/<kernel release>`，按版本缓存。
   拷贝必须用 `realpath` 解析后的路径：`/lib/modules/$(uname -r)/build` 是符号链接，
   `cp -a` 会把链接本身拷过去，编译就写进只读的原目录（`Permission denied`）；
2. 在副本里按 kbuild 自己记在 `.cmd` 里的命令，用**本机 gcc** 重建
   `fixdep`/`modpost`（含 `mk_elfconfig`、`elfconfig.h`），flags 保持内核原样；
3. 之后的构建全部用这个副本，**`KERN_DIR` 始终只读**（铁律 2）。

实测：板子上从零 `make` 到 `pud.ko` 全绿，`vermagic` 正确；副本 65 MB，删掉
`~/.cache/pud-kbuild/<release>` 即重做。x86 上交叉编不受影响（host 工具本来就能跑，
不会走副本路径）。

## 真机验证流程

### 前置

- 开发板（RK3588）+ Pico 已经插好；`lsusb` 能看到 `2e8a:0001`。
- 建议的辅助脚本（本仓库之外，工作区的 `.pud-test/` 下）：
  `ssh.sh` / `scp.sh` / `sudo.sh`（免密登录 + sudo 封装）、
  `scripts/loadN.sh`（**校验 md5 后 insmod**）。

### 部署 + 加载

`scripts/pud-load.sh` 在**板子上**跑（`insmod`/`rmmod` 只存在于那边），加载、卸载、
查看状态一个工具包完：

```bash
scp pud.ko <board>:~/pud/                     # 或者直接在板子上 make modules
ssh <board>
  scripts/pud-load.sh load                     # 显示 + 触摸
  scripts/pud-load.sh load input_only=1        # 只触摸（rmmod 随时能卸，调触摸首选）
  scripts/pud-load.sh load input_only=1 report_mode=pointer
  scripts/pud-load.sh status                   # 参数 / 显示节点 / 输入设备 / dmesg
  scripts/pud-load.sh unload                   # 被桌面占住时会告诉你重新用 --stop-gdm
  scripts/pud-load.sh unload --stop-gdm        # 停 gdm → rmmod → 起 gdm
  scripts/pud-load.sh reload input_only=1      # 换参数/换 .ko 时用
```

它替你做掉两件以前靠人记的事：

1. **vermagic 校验**：`modinfo -F vermagic` 必须等于 `uname -r`，否则直接拒绝加载并说明
   原因 —— 这代替了以前"人工对 md5"，而且能同时抓住"传了旧 `.ko`"和"编错了内核"两类事故（
   实测：`vermagic 6.1.172 (running kernel: 6.1.172)` 通过，不匹配时给出修复提示）。
   顺手把 md5 也打出来，便于和 `scp` 的来源对账。
2. **卸载被占用的处置**：先 `lsof /dev/dri/*` 列出占用者，再提示
   `--stop-gdm`；带该参数时按"停 gdm → rmmod → 起 gdm"走一遍，即使 rmmod 失败也会把会话
   拉回来（`rmmod ok (gdm restarted)` / gdm 之后仍是 `active`，实测）。

`PUD_KO=/path/to/pud.ko` 可以指定别的模块（默认为仓库根的 `./pud.ko`），
`MODULE=` 可以换模块名。工具只依赖 `kmod`/`lsof`/`systemctl`，不带任何本机路径。

> 老工作区里那些 `loadN.sh` 把路径和 md5 写死了，换一块板子或换一次构建就要改；
> 现在统一用这个工具。

### 期望的 dmesg

```
pud_drm_setup
pud-drm: pud_drm_alloc
pud-drm: mode: 480x320
pud-drm: pud_drm_register
[drm] Initialized pud-drm 1.0.0 ... for 7-1:1.0 on minor N
pud 7-1:1.0: [drm] fb0: pud-drmdrmfb frame buffer device
sn : 0x................
input: pud touch panel as /devices/.../inputNN
pud-drm: pud_drm_pipe_enable
```

### 检查清单

```bash
# 1) 有没有 oops / WARN / DMA 相关错误
dmesg | grep -iE 'pud|usb.*(fail|error)|swiotlb|rejecting|on stack|Oops|WARNING'

# 2) 显示设备是否注册并被点亮
ls /dev/dri/card*; ls /dev/fb*
cat /sys/class/drm/card*-USB-*/status    # connected
cat /sys/class/drm/card*-USB-*/enabled   # enabled

# 3) 模块引用计数（关系到能否 rmmod）
cat /sys/module/pud/refcnt
```

### 卸载模块的坑

`rmmod pud` 很可能失败：

```
ERROR: Module pud is in use
```

因为合成器（gnome-shell 那类 Wayland 合成器）持有 `/dev/dri/cardN` 的 fd。`refcnt` 会随着
分辨率/热插拔事件累积（曾观察到涨到 32）。

**先搞清楚是谁占的**，两种情形处理方式完全不同（2026-09 实测）：

| 谁占的 | 怎么认 | 怎么办 |
| --- | --- | --- |
| **用户态**会话持有 fd | `/proc/*/fd` 扫出进程（见下），`refcnt` 与"打开的 fd 数"对得上 | 停掉那个会话：`systemctl stop <unit>`（unit 名随板子不同；`pud-load.sh unload --stop-gdm` 只在那个会话**就是 gdm** 时才管用） |
| **内核内部**（fbdev 模拟 + fbcon） | 扫 `/proc/*/fd` 为空但 `refcnt > 0`；`/sys/class/vtconsole/vtcon1/name` = `frame buffer device` 且 `bind=1` | **解绑 vtconsole**：`echo 0 > /sys/class/vtconsole/vtcon1/bind`（见 README 的"Useful commands"） |

**先确认板上有没有 `lsof`**：本机就**没装**，`lsof … 2>/dev/null` 会给出"没人持有"的
**假结论**（实测踩过）。用这个不依赖工具的扫法：

```bash
for f in /dev/dri/card* /dev/fb*; do
  for p in /proc/[0-9]*; do
    for fd in $p/fd/*; do
      [ "$(readlink $fd 2>/dev/null)" = "$f" ] && \
        echo "$f <- pid $(basename $p) $(cat $p/comm 2>/dev/null)"
    done
  done
done
```

实测（2026-09）：`refcnt` 是 5，扫出来正好是一个全屏合成器会话持有的若干 card3 fd 加
`systemd-logind` 的一个 —— **fbcon 不占 fd、也不拦 `rmmod`**（`/dev/fb0` 一个进程都没打开）。

**可靠的重置手段仍然是重启开发板。**

### 只调触摸：`input_only=1`

反复试触摸时别把显示那半边也加载进来：

```bash
sudo insmod pud.ko input_only=1         # 只注册 input 设备，没有 DRM/fbdev 节点
grep -A5 pud /proc/bus/input/devices    # 找 eventN
sudo timeout 10 cat /dev/input/eventN | od -An -tx2   # 按屏幕就会出字节
sudo rmmod pud                          # 立刻能卸
```

没有 DRM 节点 → gnome-shell/logind 占不住模块 → 不用停 gdm、不用重启板子，
改一次 `input.c` 就能马上重编重载。默认（`input_only=0`）仍是显示 + 触摸一起注册，
那时 `rmmod` 还是会被桌面会话挡住。

### fbdev 编号不固定

PUD 是哪个 `/dev/fbN` **取决于启动顺序**：板载 `rockchipdrmfb` 和 `pud-drmdrmfb`
谁先注册谁拿 `fb0`。曾经 PUD 是 `fb1`，后来又变成 `fb0`。**别在脚本里写死**，
用名字查：

```bash
for f in /sys/class/graphics/fb*; do echo "$f: $(cat $f/name)"; done
# /sys/class/graphics/fb0: pud-drmdrmfb     ← 这个才是 PUD
```

## 调试固件（Pico）

驱动调试不需要动固件；如果要看固件内部状态（解码计数、断点），走 CMSIS-DAP：

- OpenOCD 在 **Windows 宿主机**上跑（WSL 看不到 USB 设备，也没有 `/dev/bus/usb` 权限，
  且无法 `mknod`）：
  ```bash
  openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 10000"
  ```
- WSL 侧通过 `localhost:3333`（gdb）连过去：
  ```bash
  gdb-multiarch -q -nh \
    -ex "file Pico-USB-Display/build-pico2/pico-usb-display.elf" \
    -ex "target extended-remote localhost:3333"
  ```
- **原生 Linux 开发机不需要 Windows/WSL 这一层**（2026-09 实测）：调试器直接挂在开发机上时，
  OpenOCD 就跑在本机，gdb 连 `localhost:3333` 的命令与上面完全相同。此时注意 openocd 0.12
  把 `rp2350.cm0` / `rp2350.cm1` 当成**一个 SMP 组**：只 halt 一个核再 `resume` 会失败，
  并把核留在停机状态（板子看起来卡死）—— 先把两个核都 `halt`，再 `resume` 一次带上整组。
  详见固件仓 `Pico-USB-Display/notes/debugging.md` 的"halt/resume 的坑"。
- `/tmp` 在 WSL 里**每次调用都是独立的**，不要把中间产物放那儿再跨调用读。

详见 `Pico-USB-Display/notes/debugging.md`。

## 其它环境注意事项

- 交叉编译器：`aarch64-linux-gnu-gcc`（Debian/Ubuntu 包 `gcc-aarch64-linux-gnu`）。
- 若 `rmmod` 后立刻 `insmod` 报 `File exists`，说明上一次卸载没干净 —— 重启板子。
- 编译产物（`*.o`、`*.ko`、`*.mod*`、`build/`）已在 `.gitignore` 中，不要提交。
