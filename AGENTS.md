# AGENTS.md

## Skills（本仓遵守）

本仓的一切工作遵循工作区 `../AGENTS.md` 约定的四份 skill。**摘要随仓携带**（离线可读），
完整版在工作区 `skills/`。

| skill | 本仓副本 | 一句话 |
| --- | --- | --- |
| Repository Exploration | [`skills/developer-repository-exprolation/Summary.md`](skills/developer-repository-exprolation/Summary.md) | 先理解再修改；证据优先于直觉 |
| Knowledge | [`skills/developer-knowledge/Summary.md`](skills/developer-knowledge/Summary.md) | 首屏结论、事实分级、信息预算、漂移检查 |
| Testing | [`skills/developer-testing/Summary.md`](skills/developer-testing/Summary.md) | tests/tools 分层、oracle 声明、退出码、N 次测量 |
| Code Quality | [`skills/developer-code-quality/Summary.md`](skills/developer-code-quality/Summary.md) | **能跑 ≠ 完成**；可读性有硬标准 |

### 动手前的四行闸门（**强制**）

改任何代码或配置**之前**先写出这四行 ✓。**第 1 行或第 4 行写不出来就停手** ✗ —— 那是在猜 ✗。

```text
已验证：<确认了什么，凭据是什么：代码/实测/构建日志>
仍未知：<还没确认的；不许用推测填空>
最小改动：<只改一处，为什么是这一处>
生效验证：<如何证明改动真的生效：探针 / grep 生成物 / 构建日志里的编译行>
```

**先确认仪器，再相信读数** ✓ —— 宏没被注入、文件没被编译、配置被 defconfig 覆盖，
这三件事的症状都是"结果莫名其妙" ✗。


> 本文件只写 PUD-kernel-drivers **特有**的铁律、架构不变量与构建/验证入口。
> 知识库与测试的**通用约定**（文档结构、信息预算、事实分级、oracle 声明、CLI/退出码、命名、
> 文档↔代码漂移检查）以工作区根 [`../AGENTS.md`](../AGENTS.md) 为准，这里不再重复。
> 详细知识在 [`notes/`](notes/README.md)。

## 铁律

1. **未经明确指令，不要 `git commit`，更不要 `git push`。** 改完先报告改了什么、验证到什么程度，等指令。
2. **绝不修改 `KERN_DIR` 指向的内核源码目录。** 它只作为头文件/符号来源，所有改动留在本仓库；
   需要 objtree 就传 `KERN_OBJ_DIR`，不要去源码树里补文件。
3. **不要把内核路径写进 Makefile 或任何文件。** 一律命令行传入：
   `KERN_DIR=` / `KERN_OBJ_DIR=` / `ARCH=` / `CROSS_COMPILE=`。
4. **仓库内不得出现内网/个人信息**：本机绝对路径（`/home/...`）、内网 IP、口令、内部项目代号。
   文档里用 `<vendor-kernel-src>` 这类占位符。
5. **优先保证内核不崩。** 宁可功能不完整，也不要 oops/panic/WARN。
6. **不要往本机内核 `insmod`。** 本机安全验证只走 `make qemu`；真机加载只在板子上做。

## 提交与身份

- `user.name` = `Wooden Chair`，`user.email` = `hua.zheng@embeddedboys.com`
- **提交一律带 `Signed-off-by`**：用 `git commit -s`（仓库既有历史都带 sign-off）
- 提交信息用**内核风格**：`模块: 组件: 简述`，正文写清具体改了什么、为什么、效果；
  一个逻辑改动一个提交。
- 分支：上游主线 `kernel-6.12`；本项目的 6.1 移植分支 `rk-6.1.172`；本机 generic 内核的
  `7.0.0-34-generic`（名字跟运行内核走，内核升级后改名）。

## 架构不变量（动了就坏）

1. **USB 传输的 buffer 必须 DMA 可映射。** 不能是栈（触发 `WARN ... transfer buffer is on stack`，
   传输被拒 `-EAGAIN`），不能是 vmalloc（`rejecting DMA map of vmalloc memory`）。
   - `pud->ctrl_buf`（嵌在 `struct pud` 里 → 堆）✓
   - `pud->encoder_buf` 是 EP1 的 DMA 源，**必须 `dma_alloc_coherent`** ✓
   - `pud->tx_buf` 只给 CPU 用，`vmalloc` 可以 ✓
   - `pud_flush()` 里栈上的 `struct pud_usb_bulk_context`（同步路径）与 `struct pud_ep1_async_ctx`
     （异步路径）是**描述符不是 buffer**，合法 —— 别"顺手"改成堆分配。
   - 完整自查表与条件链见 [pitfalls.md](notes/pitfalls.md) 一。
2. **`pud_drm_alloc()` 失败返回 `ERR_PTR`，必须 `IS_ERR()` 判断**，不能用 `if (!drm)`。
3. **一帧的压缩结果必须 ≤ 设备上报的单次传输上限**，靠 `pud_fb_dirty()` 的分带
   （`pud->max_band_pixels`）保证。上限**由设备通过 `PUD_CMD_GET_CAPS` 上报**
   （真机日志 `caps: proto 2, frame_max 65536, decoder 3 -> 21835 pixels per band`；
   `USB_TRANS_MAX_SIZE` 是 65535，`min()` 之后落到 21835），拿不到时退回
   `PUD_DEFAULT_BAND_PIXELS`（21835 px）。**不要改回写死的常量** —— RP2040 的固件只接受一半大小的传输。
   同一个命令还上报**面板参数**（分辨率/旋转/bpp/总线时钟/触摸轮询周期），DRM mode、`encoder_buf`、
   fbdev 显存、输入设备轴范围都由它推出来 —— **不要在驱动里写死 480×320**。
   改分带规则前先读 [display-and-refresh.md](notes/display-and-refresh.md)。
4. **flush 失败必须置 `needs_full_refresh`**，否则那块 damage 永久丢失 = 残影。
5. **协议字段改动要成对改固件**（`REQ_*`、`struct pud_ep1_header`、`struct req_ep2_in`），
   并同步两个仓库的 `notes/usb-protocol.md`。
6. **EP4 触摸只保持一条常驻 URB，而且触摸在设备侧是可选的。**
   `PUD_CAPS_TOUCH`（capability flags bit0）没置位时**不要注册输入设备** —— 那种固件 EP4 永远不发帧，
   老驱动会给用户一个"存在但永远不动"的输入设备。设备主动推送：`pud_input_setup()` 提交一次，
   完成回调里**除 `-ENOENT`/`-ECONNRESET`/`-ESHUTDOWN`（正在卸载 / 总线没了）外，无论什么 status
   都要重新提交**；旧版 `if (urb->status) return;` 遇到一次抖动就永久不再提交，输入就此失灵。
   回调在中断上下文，重提交用 `GFP_ATOMIC`。**不要再为每个样本发 `REQ_EP4_IN` 控制请求**（旧协议做法）。
   注册成什么设备由 `report_mode` 决定（`touch` 默认 / `pointer`）；**绝对设备必须给分辨率**
   （`input_abs_set_res()`，用设备上报的 `width_mm/height_mm`），否则 libinput 判定为驱动 bug。
   清理顺序：`usb_kill_urb()`（等回调结束）→ `usb_free_urb()` → `kfree(ep_int_buf)`；`pud->indev`
   来自 `devm_input_allocate_device()`，**不要再手动 `input_unregister_device()`**（会重复注销）。
   细节见 [input-touch.md](notes/input-touch.md)。

## 构建入口

三种模式（默认取本机运行内核），细节见 [build-and-test.md](notes/build-and-test.md)：

| 目标 | 命令要点 |
| --- | --- |
| 厂商 6.1 源码树（objtree 模式） | `make modules KERN_DIR=<vendor-kernel-src> KERN_OBJ_DIR=<vendor-kernel-objtree>` |
| 板子运行内核的 headers | `make -C <headers> M=$PWD ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules` |
| 本机内核（x86-64） | `make modules` |

- 构建开关 `PUD_USB_ASYNC`：`0`（默认）= 同步 `usb_sg`；`1` = 异步 URB。两条路径状态不共享，
  一个镜像只编一条；对比结论见 [build-and-test.md](notes/build-and-test.md)。
- `vermagic` 必须与目标内核 `uname -r` 一致，否则
  `disagrees about version of symbol module_layout`。验证：`modinfo pud.ko | grep vermagic`。
- 在 arm64 板上用厂商 headers 本地编时，包里的 host 工具是 x86-64（`Exec format error`）；
  Makefile 检测到就自动把 headers 拷到 `~/.cache/pud-kbuild/<release>` 用本机 gcc 重编那几个工具，
  **`KERN_DIR` 始终只读**。

## 验证入口

- **本机安全验证：`make qemu`**（[board-testing.md](notes/board-testing.md)）。它在 QEMU 客户机里加载
  模块并留 root shell，出问题是客户机重启，不是工作站。`PARAMS=`/`CMD=`/`PASSTHROUGH=` 可调。
- **宿主机离线检查**：`make test`（协议常量、分带公式、`modinfo`/vermagic 静态检查，不加载模块）。
- **板子上的加载/卸载/检查**：仓里的 [`scripts/pud-load.sh`](scripts/pud-load.sh)：
  `load [模块参数...]` / `unload [--stop-dm]` / `reload` / `status`。它先校验 `vermagic` 与 `uname -r`
  一致，并在卸载被桌面占住时列出占用者、`--stop-dm` 时按"停显示管理器（自动识别 gdm/lightdm/sddm…）
  → rmmod → 起回来"走一遍。
- **只调触摸就别加载显示**：`scripts/pud-load.sh load input_only=1` 只注册 input 设备，
  没有 DRM/fbdev 节点，桌面会话占不住模块，`unload` 立刻成功。看事件：
  `grep -A5 pud /proc/bus/input/devices` 找 event 号，再
  `sudo timeout 10 cat /dev/input/eventN | od -An -tx2`，按屏幕会看到
  `0003 0000 xxxx`（ABS_X）/ `0003 0001 yyyy`（ABS_Y）/ `0001 014a 0001`（BTN_TOUCH）。
- **没人开桌面会话时想让面板自己亮**：`scripts/pud-load.sh load initial_mode=1` 从 probe 直接提交一次
  固定 mode（默认关：它把驱动放进"没人在环里"的发送路径；前置条件见 [architecture.md](notes/architecture.md)
  的"运行期参数"）。**用完别带着它卸载**（当前已知的 teardown 卡死，见 [pitfalls.md](notes/pitfalls.md) 4.5）。
- **fbdev 编号不固定**：PUD 可能是 `fb0` 也可能是 `fb1`。用 `cat /sys/class/graphics/fb*/name`
  找 `pud-drmdrmfb`，不要写死。
- **不加载驱动也能测全部功能**：用固件仓 `Pico-USB-Display/scripts/`（用户空间 pyusb）。
- **看固件内部状态**（解码计数等）走 CMSIS-DAP。原生 Linux 开发机上 OpenOCD 就跑在本机，
  gdb 连 `localhost:3333`；注意 openocd 0.12 把两个核当 **SMP 组**，`resume` 前必须先把两个核都
  `halt`（否则 resume 失败并把核留在停机状态）。只读检查后 `monitor resume`，**别用
  `monitor reset run`**（会清状态）。详见固件仓 `Pico-USB-Display/notes/debugging.md`。

### EP1 失败时的调试纪律

- **当前实现**：固件对不可信 header（`12+size` 超限、矩形越界）是**丢弃并重新武装**
  （`g_ep1_stat.oversize++`，宿主看不到错误），不再 stall EP1；`pud_flush()` 里那段
  `usb_clear_halt()` 现在**只是防御**（见 `usb.c` 注释）。用临时超限补丁验证过走的就是丢弃分支。
- **历史教训（老固件 / 控制器级 stall）**：一次 EP1 stall 足以把主机控制器卡住 —— 之后
  `usb_sg_wait()`/`usb_sg_cancel()` 不返回，DRM modeset 锁被占住，`/sys/kernel/debug/dri/*/state`
  都读不出来，界面永久黑屏。所以**别靠反复 reload 试探**，EP1 连续失败就先卸载再查。
  这条纪律在正常路径上已够不到触发点，但换回老固件或遇到控制器级 stall 时仍然适用。
- 下结论前**两侧对账**：主机 `dmesg`/`usbmon`（[usbmon.md](notes/usbmon.md)）与设备侧计数器。

## 代码约定

- C 风格用**内核风格：tab + 8 宽缩进**（仓库根 `.clang-format` 取自内核，唯一偏离是
  `UseTab: ForIndentation`），`u8/u16/u32` 是内核类型别名。**vendored 的 `rgb565_qoi.*` /
  `rgb565_rle.*` / `jpegenc.*` 不要格式化**（要与上游逐字节一致）。
- **注释体不会被 clang-format 修**（`ReflowComments: false`）：注释续行必须**手写**成
  "每层一个 tab + 一个空格"（`\t * 文本`、`\t */`），格式化对错误缩进是 no-op。
- 新增源文件要加进 `Makefile` 的对象列表（`$(MODULE_NAME)-y += ...`，`MODULE_NAME:=pud`）。
- 收尾自查：`make modules` 没有新增 warning，`dmesg` 里没有 WARN/oops。
- `make modules` 会顺带生成 `compile_commands.json`（`.gitignore` 已忽略），仓库根 `.clangd`
  指向它；交叉编译器头文件路径报缺失时用 `clangd --query-driver=/usr/bin/aarch64-linux-gnu-*`。

## 文档维护

- 知识库在 [`notes/`](notes/README.md)。改了行为就同步对应文档；协议改动要同时改固件仓镜像文档。
- **每次更新知识库都要做文档↔代码漂移检查**：默认值、常量、开关、**特性是否存在**逐条核对，
  以代码/实测为准；过时测量标注"在配置 X 下测得"而非删除（见 [`../AGENTS.md`](../AGENTS.md) 一）。
- 只写**已验证**的结论；推测显式标注"未验证"。中文叙述，命令/路径/标识符保留英文。

## 驱动工具的方式（与工作区规范同源）

- **不许盲目 `sleep`，不许 blanket 超时** ✓ —— 用**轮询就绪**（0.2 s 间隔）+ **秒级超时** ✓。
  硬件测试必须**显式定义就绪检测**，不要依赖"设备恰好已经跑着" ✓。
- 反例：`sleep 22` + `timeout 300` ⇒ 明明 0.4 s 就有结论的操作拖到几分钟 ✗。
- 正解：`usb.core.find` 轮询 ✓、控制请求 0.5 s 超时 ✓、shell 命令 `timeout 10` ✓。
