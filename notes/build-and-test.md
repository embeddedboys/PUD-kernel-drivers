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

## 两种构建模式

### A. 厂商内核源码树（6.1.118 分支，objtree 模式）

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

步骤：

```bash
# 1) 从板子取 headers（板子上有 /usr/src/linux-headers-$(uname -r)）
tar czf hdrs-$(uname -r).tgz -C /usr/src linux-headers-$(uname -r)

# 2) 在 x86-64 开发机上解出（见下面的"为什么不能直接在板子上编"）
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

### ⚠️ 为什么不能直接在板子上编：headers 里的 host 工具是 x86-64

板子 `/usr/src/linux-headers-6.1.172/scripts/basic/fixdep` 的实际类型是：

```
ELF 64-bit LSB pie executable, x86-64 ... for GNU/Linux 3.2.0
```

因为厂商是**在 x86-64 主机上交叉编译**出 arm64 内核的，`fixdep`/`modpost` 这类
**host 工具**自然编成了 x86-64。在 arm64 板子上执行会直接 `Exec format error`。
所以正确做法是：**把 headers 拷到 x86-64 主机上，用交叉编译器编**。
（拷贝到 WSL 后 `fixdep` 的 BuildID 与板子上的完全一致，可直接运行。）

## 真机验证流程

### 前置

- 开发板（RK3588）+ Pico 已经插好；`lsusb` 能看到 `2e8a:0001`。
- 建议的辅助脚本（本仓库之外，工作区的 `.pud-test/` 下）：
  `ssh.sh` / `scp.sh` / `sudo.sh`（免密登录 + sudo 封装）、
  `scripts/loadN.sh`（**校验 md5 后 insmod**）。

### 部署 + 加载

```bash
scp pud.ko <board>:~/pud-test/pud.ko
# 板子上：校验 md5 → insmod → 打印 dmesg
ssh <board> '~/pud-test/loadN.sh'
```

用 md5 校验是刻意的：曾经因为传了旧文件而白折腾一整轮。

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

因为 gnome-shell（Wayland 合成器）持有 `/dev/dri/cardN` 的 fd。`refcnt` 会随着
分辨率/热插拔事件累积（曾观察到涨到 32）。**可靠的重置手段是重启开发板**。

次选手段（不一定管用）：
- `systemctl stop gdm` 之类停掉会话，再 `rmmod`
- 解绑 vtconsole：`echo 0 > /sys/class/vtconsole/vtcon1/bind`

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
- `/tmp` 在 WSL 里**每次调用都是独立的**，不要把中间产物放那儿再跨调用读。

详见 `Pico-USB-Display/notes/debugging.md`。

## 其它环境注意事项

- 交叉编译器：`aarch64-linux-gnu-gcc`（Debian/Ubuntu 包 `gcc-aarch64-linux-gnu`）。
- 若 `rmmod` 后立刻 `insmod` 报 `File exists`，说明上一次卸载没干净 —— 重启板子。
- 编译产物（`*.o`、`*.ko`、`*.mod*`、`build/`）已在 `.gitignore` 中，不要提交。
