# 真机与 QEMU 验证

> 本机安全验证只走 `make qemu`（QEMU 客户机里加载模块，出问题是客户机重启）；
> 板子上的加载/卸载/检查用 `scripts/pud-load.sh`。**不要往本机内核 `insmod`**。

## TL;DR

- `make qemu` = virtme-ng 起客户机 + 自动加载 `pud.ko` + 留 root shell；`PARAMS=`/`CMD=`/`PASSTHROUGH=` 可调。
- 客户机里的 `vermagic` 必须与客户机内核一致，`scripts/qemu.sh` 会自动按选中的内核重编模块
  （这会覆盖仓库里的 `./pud.ko`，之后 `make modules` 编回运行内核）。
- 板子上加载前先校验 `vermagic`；卸载被桌面占住时用 `--stop-dm`。
- 只调触摸用 `input_only=1`：没有 DRM 节点，桌面占不住模块，`rmmod` 立刻成功。
- 编译/构建选项见 [build-and-test.md](build-and-test.md)。

## QEMU（`make qemu`）

```bash
make qemu                          # 启动 + 加载 + 交互 shell
make qemu CMD='dmesg | grep pud'   # 跑一条命令就退出
make qemu PARAMS=report_mode=pointer
make qemu PASSTHROUGH=0            # 不把面板交给客户机
```

`scripts/qemu.sh` 背后做的事（出问题时照这个手工来）：

- **挑客户机内核**：优先运行内核，但得看得到镜像 —— Ubuntu 的 `/boot/vmlinuz-*` 是 root:600，
  拿不到就退到"能从 apt 缓存里的 `linux-image-*.deb` 解出来"的最新一个（缓存在
  `~/.cache/pud-kbuild/img/`）。固定某个镜像传 `KERNEL_IMG=/path/to/vmlinuz-<rel>`；想让客户机跑
  **运行内核**，把它的镜像复制成可读的一份：
  `sudo cp /boot/vmlinuz-$(uname -r) ~/.cache/pud-kbuild/img/`。
- **按客户机内核编模块**：`vermagic` 必须一致，定下内核后用 `/lib/modules/<rel>/build` 重编
  （`modinfo -F vermagic` 已经是它了就跳过）。
- **透传面板**：`lsusb` 看得到 `2e8a:0001` 时加 `-device usb-host,...`（本机没加载 pud，没有驱动占着它）。
  客户机里的驱动于是**真的在推那块面板**：probe → caps → DRM 注册 → fbcon 接管 → 写 `/dev/fb0`
  能在 usbmon 里数到 EP1，`rmmod` 干净。
- **首次 probe 的 caps 兜底**：设备刚交给客户机时**第一笔厂商请求可能超时**
  （`no capability report (-110)`），驱动退回宿主机默认值。同一个请求从 host 问设备是好的、重载一次也
  一定好，所以那是**模拟 xHCI 侧的时序**、不是驱动行为：脚本检测到就自动 `rmmod` + `insmod` 一次把
  真实 caps 拿回来。手工路线遇到它，重载一次即可。

手工起 VM 时几个容易卡住的地方：

- `vng` **没有** `--kernel` 参数。内核镜像走 `--run <bzImage>`；`--run` 不带参数就用本机运行的内核。
- `.venv/bin` 要在 `PATH` 里：`vng` 靠它找同目录的 `virtme-run`。
- 客户机里 `insmod` 之前先 **`modprobe drm_dma_helper`**。7.0 把 fbdev 客户端并进了 drm 核心，
  模块引用的 `drm_fbdev_dma_driver_fbdev_probe` 出自那里；不先加载就会
  `insmod: ERROR: could not insert module pud.ko: Unknown symbol in module`。
- 客户机里的 `/tmp` 是它自己的，客户机一退出就没了。要把文件带出来用 `--rwdir <hostdir>`
  （两边同一个路径，客户机可写）；`dmesg -w` 这类"边跑边写"的输出要套 `stdbuf -oL`，否则块缓冲在
  机器断电时全丢。
- 客户机里没有 systemd 当 PID 1（`systemctl poweroff` 不可用），要关机在 `--exec` 里跑一句
  `python3 -c "import ctypes; ctypes.CDLL(None, use_errno=True).reboot(0x4321fedc)"`。

实测（2026-09，7.0.0-34 客户机 + 真设备直通）：写 `/dev/fb0` 64 KB 随机像素 → EP1 7 笔 ~95 KB
（QOI 编码，见 [display-and-refresh.md](display-and-refresh.md)）；`poweroff` 时客户机 dmesg 末尾出现
`pud_drm_pipe_disable`（`usb_driver.shutdown` 那条路径）。

## 板子上的验证流程

### 前置

- 开发板（RK3588）+ Pico 已插好；`lsusb` 能看到 `2e8a:0001`。
- 建议的辅助脚本（本仓库之外，工作区 `.pud-test/` 下）：`ssh.sh` / `scp.sh` / `sudo.sh`
  （免密登录 + sudo 封装）、`scripts/loadN.sh`（校验 md5 后 insmod）。

### 部署 + 加载

`scripts/pud-load.sh` 在**板子上**跑（`insmod`/`rmmod` 只存在于那边）：

```bash
scp pud.ko <board>:~/pud/                     # 或者直接在板子上 make modules
ssh <board>
  scripts/pud-load.sh load                     # 显示 + 触摸
  scripts/pud-load.sh load input_only=1        # 只触摸（rmmod 随时能卸，调触摸首选）
  scripts/pud-load.sh load input_only=1 report_mode=pointer
  scripts/pud-load.sh status                   # 参数 / 显示节点 / 输入设备 / dmesg
  scripts/pud-load.sh unload                   # 被桌面占住时会提示重新用 --stop-dm
  scripts/pud-load.sh unload --stop-dm         # 停显示管理器 → rmmod → 起回来
  scripts/pud-load.sh reload input_only=1      # 换参数/换 .ko 时用
```

它替你做掉三件以前靠人记的事：

1. **vermagic 校验**：`modinfo -F vermagic` 必须等于 `uname -r`，否则直接拒绝加载并说明原因 ——
   同时抓住"传了旧 `.ko`"和"编错了内核"两类事故（实测：匹配时通过，不匹配时给出修复提示）。
   顺手把 md5 也打出来，便于和 `scp` 来源对账。
2. **卸载被占用的处置**：先 `lsof /dev/dri/*` 列出占用者，再提示 `--stop-dm`；带该参数时按
   "停显示管理器 → rmmod → 起回来"走一遍，即使 rmmod 失败也会把会话拉回来。显示管理器是
   `systemctl is-active` 问出来的（gdm/lightdm/sddm…，`PUD_DM=<unit>` 可覆盖）。
3. **依赖先加载**：`insmod` 自己不会解析依赖，而 7.0 把 fbdev 客户端放进 drm 核心、DMA helper 放在
   `drm_dma_helper` —— 脚本按 `modinfo -F depends` 先 `modprobe -a`。漏掉就是
   `Unknown symbol in module`（见 [pitfalls.md](pitfalls.md) 4.4）。

`PUD_KO=/path/to/pud.ko` 可以指定别的模块（默认仓库根的 `./pud.ko`），`MODULE=` 可换模块名。
工具只依赖 `kmod`/`lsof`/`systemctl`，不带任何本机路径。

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

`rmmod pud` 很可能失败：`ERROR: Module pud is in use`。原因是合成器（gnome-shell 那类 Wayland
合成器）持有 `/dev/dri/cardN` 的 fd；`refcnt` 会随分辨率/热插拔事件累积（曾观察到涨到 32）。

**先搞清楚是谁占的**，两种情形处理方式完全不同（2026-09 实测）：

| 谁占的 | 怎么认 | 怎么办 |
| --- | --- | --- |
| **用户态**会话持有 fd | `/proc/*/fd` 扫出进程，`refcnt` 与"打开的 fd 数"对得上 | 停掉那个会话：`pud-load.sh unload --stop-dm` 会自己找运行中的显示管理器停掉再卸；手动是 `systemctl stop <unit>` |
| **内核内部**（fbdev 模拟 + fbcon） | 扫 `/proc/*/fd` 为空但 `refcnt > 0`；`/sys/class/vtconsole/vtcon1/name` = `frame buffer device` 且 `bind=1` | **解绑 vtconsole**：`echo 0 > /sys/class/vtconsole/vtcon1/bind` |

**先确认板上有没有 `lsof`**：本机就**没装**，`lsof … 2>/dev/null` 会给出"没人持有"的**假结论**
（实测踩过）。用这个不依赖工具的扫法：

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
`systemd-logind` 的一个 —— **fbcon 不占 fd、也不拦 `rmmod`**。

**可靠的重置手段仍然是重启开发板。**

### 只调触摸：`input_only=1`

```bash
sudo insmod pud.ko input_only=1         # 只注册 input 设备，没有 DRM/fbdev 节点
grep -A5 pud /proc/bus/input/devices    # 找 eventN
sudo timeout 10 cat /dev/input/eventN | od -An -tx2   # 按屏幕就会出字节
sudo rmmod pud                          # 立刻能卸
```

按屏幕会看到 `0003 0000 xxxx`（ABS_X）/ `0003 0001 yyyy`（ABS_Y）/ `0001 014a 0001`（BTN_TOUCH）。
没有 DRM 节点 → 桌面会话/logind 占不住模块 → 不用停显示管理器、不用重启。默认（`input_only=0`）
仍是显示 + 触摸一起注册，那时 `rmmod` 会被桌面会话挡住。

### fbdev 编号不固定

PUD 是哪个 `/dev/fbN` **取决于启动顺序**：板载 `rockchipdrmfb` 和 `pud-drmdrmfb` 谁先注册谁拿 `fb0`。
曾经 PUD 是 `fb1`，后来又变成 `fb0`。**别在脚本里写死**，用名字查：

```bash
for f in /sys/class/graphics/fb*; do echo "$f: $(cat $f/name)"; done
# /sys/class/graphics/fb0: pud-drmdrmfb     ← 这个才是 PUD
```

## 调试固件（Pico）

驱动调试不需要动固件；要看固件内部状态（解码计数、断点）走 CMSIS-DAP：

- OpenOCD 在 **Windows 宿主机**上跑（WSL 看不到 USB 设备，也没有 `/dev/bus/usb` 权限，且无法 `mknod`）：
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
  OpenOCD 就跑在本机，gdb 连 `localhost:3333` 的命令与上面完全相同。注意 openocd 0.12 把
  `rp2350.cm0` / `rp2350.cm1` 当成**一个 SMP 组**：只 halt 一个核再 `resume` 会失败，并把核留在
  停机状态；先把两个核都 `halt`，再 `resume` 一次带上整组。详见固件仓
  `Pico-USB-Display/notes/debugging.md` 的"halt/resume 的坑"。
- **只读检查后要 `monitor resume`，别用 `monitor reset run`**（会清状态）。
- `/tmp` 在 WSL 里**每次调用都是独立的**，不要把中间产物放那儿再跨调用读。

## 省时间的板子工作方式

1. **一轮只做一件事**：脚本先写好，一次 `scp` 上去跑完 —— 不要在一轮里串多次 ssh、gdb、
   `make modules`。板子一卡，一轮能白等十分钟。
2. **可能挂住的命令一律套 `timeout`**（`lsusb`、`dmesg`、debugfs 读写、`make`、`rmmod`）：
   USB 栈一卡，`lsusb` 会永远不返回。
3. **gdb 读固件是 30~60 s 级**：一轮最多读一次，能用 `dmesg` 说清就不读。
4. **驱动只编译一次**：改完一次 `make modules`，`pud.ko` 留在板子上复用。
5. 板子重启后**总线与路径会变**（`6-1` → `3-1`）：脚本里动态发现，别写死接口路径。
6. 下结论前**两侧对账**：主机 `dmesg`/`usbmon`（见 [usbmon.md](usbmon.md)）与设备侧计数器。
