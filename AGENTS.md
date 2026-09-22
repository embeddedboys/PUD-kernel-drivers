# AGENTS.md

本仓库的工作规则，供 AI agent（以及人）在改动前先读一遍。

**详细知识在 [`notes/`](notes/README.md)**：本文只写"必须遵守的约束"和入口，
不重复细节，以免每次会话都吃掉大量上下文。

---

## 铁律

1. **未经明确指令，不要 `git commit`，更不要 `git push`。**
   改完先报告改了什么、验证到什么程度，等指令。
   （曾经把"告诉你提交者身份"误解成"让你提交"，多做了事。）
2. **绝不修改 `KERN_DIR` 指向的内核源码目录。** 它只作为头文件/符号来源，
   所有改动留在本仓库。需要 objtree 就传 `KERN_OBJ_DIR`，不要去源码树里补文件。
3. **不要把内核路径写进 Makefile 或任何文件。** 一律命令行传入：
   `KERN_DIR=` / `KERN_OBJ_DIR=` / `ARCH=` / `CROSS_COMPILE=`。
4. **仓库内不得出现内网/个人信息**：本机绝对路径（`/home/...`）、内网 IP、
   口令、内部项目代号。文档里用 `<vendor-kernel-src>` 这类占位符。
5. **优先保证内核不崩。** 宁可功能不完整，也不要 oops/panic/WARN。

## 提交与身份

- `user.name` = `Wooden Chair`，`user.email` = `hua.zheng@embeddedboys.com`
- **提交一律带 `Signed-off-by`**：用 `git commit -s`（仓库既有历史都带 sign-off）
- 提交信息用**内核风格**：`模块: 组件: 简述`，正文写清具体改了什么、为什么、效果；
  一个逻辑改动一个提交，不要把互不相关的改动塞进同一个提交
- 分支：上游主线 `kernel-6.12`；本项目的 6.1 移植分支 `rk-6.1.118`

## 构建（两种模式，别混）

| 目标 | 命令要点 |
| --- | --- |
| 厂商 6.1 源码树（objtree 模式） | `make modules KERN_DIR=<vendor-kernel-src> KERN_OBJ_DIR=<vendor-kernel-objtree>` |
| 板子运行内核的 headers | `make -C <headers> M=$PWD ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules` |

- `vermagic` 必须与板子 `uname -r` 一致，否则 `disagrees about version of symbol module_layout`。
- **在板子上本地编也能用**（`make modules`，KERN_DIR 默认就是 `/lib/modules/$(uname -r)/build`）。
  厂商的 headers 包里 host 工具（`fixdep`/`modpost`）是 **x86-64** 的，在 arm64 板上会
  `Exec format error`，而那个目录 root 只读、又缺 Kconfig，kbuild 自己重建不了；Makefile 检测到
  这种不匹配会自动把 headers 复制到 `~/.cache/pud-kbuild/<release>` 并在副本里用本机 gcc 重编
  这几个工具（命令取自 kbuild 自己的 `.cmd`），**KERN_DIR 始终只读**。副本按内核版本缓存，
  删掉该目录即重做。
- 在 x86 开发机上交叉编译同样可行（`KERN_DIR=<vendor-kernel-src> KERN_OBJ_DIR=<vendor-kernel-objtree>`
  或板子 headers），此时 host 工具本来就能跑，不会走上面的副本路径。
- 验证面：`modinfo pud.ko | grep vermagic`、`dmesg | grep -i pud`。
  细节见 [`notes/build-and-test.md`](notes/build-and-test.md)。

可调构建开关：

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `PUD_USB_ASYNC` | `0` | EP1 传输路径：`0` = 同步 `usb_sg`（`usb_sg_init`/`usb_sg_wait`），`1` = 异步 URB（`usb_submit_urb` + completion，`usb_kill_urb` 取消）。两条路径状态不共享，一个镜像只编一条；对比结论见 [`notes/build-and-test.md`](notes/build-and-test.md) |

## 架构不变量（动了就坏）

1. **USB 传输的 buffer 必须 DMA 可映射。** 不能是栈（`object_is_on_stack`），
   不能是 vmalloc（`is_vmalloc_addr`）。栈 buffer 会触发内核
   `WARN ... transfer buffer is on stack` 且传输被拒（`-EAGAIN`）。
   - `pud->ctrl_buf`（嵌在 `struct pud` 里 → 堆）✓
   - `pud->encoder_buf` 是 EP1 的 DMA 源，**必须 `dma_alloc_coherent`** ✓
   - `pud->tx_buf` 只给 CPU 用，`vmalloc` 可以 ✓
   - `pud_flush()` 里栈上的 `struct pud_usb_bulk_context`（同步路径）与
     `struct pud_ep1_async_ctx`（异步路径）是**描述符不是 buffer**，
     合法 —— 别"顺手"改成堆分配。
2. **`pud_drm_alloc()` 失败返回 `ERR_PTR`，必须 `IS_ERR()` 判断**，不能用 `if (!drm)`。
3. **一帧的压缩结果必须 ≤ 设备上报的单次传输上限**，靠 `pud_fb_dirty()` 的分带
   （`pud->max_band_pixels`）保证。这个上限**由设备通过 `PUD_CMD_GET_CAPS` 上报**
   （真机日志 `caps: proto 2, frame_max 65536, decoder 3 -> 21835 pixels per band`；
   `USB_TRANS_MAX_SIZE` 是 65535，所以 `min()` 之后落到 21835），
   拿不到时退回 `PUD_DEFAULT_BAND_PIXELS`（21835 px）。
   **不要把它改回写死的常量** —— RP2040 的固件只接受一半大小的传输。
   同一个命令还上报**面板参数**（分辨率/旋转/bpp/总线时钟/触摸轮询周期），
   DRM mode、`encoder_buf`、fbdev 显存、输入设备轴范围都由它推出来 ——
   **不要在驱动里写死 480×320**（见 [`notes/architecture.md`](notes/architecture.md) 的"设备参数"）。
   改分带规则前先读 [`notes/display-and-refresh.md`](notes/display-and-refresh.md)。
4. **flush 失败必须置 `needs_full_refresh`**，否则那块 damage 永久丢失 = 残影。
5. **协议字段改动要成对改固件**（`REQ_*`、`struct pud_ep1_header`、`struct req_ep2_in`），
   并同步两个仓库的 `notes/usb-protocol.md`。
6. **EP4 触摸只保持一条常驻 URB，而且触摸在设备侧是可选的。**
   `PUD_CAPS_TOUCH`（capability flags 的 bit0）没置位时**不要注册输入设备** ——
   `pico-display-lib` 里多数板级配置是 `INDEV_DRV_NOT_USED=1`，那种固件 EP4 永远不发帧，
   老驱动会给用户一个"存在但永远不动"的输入设备。设备主动推送：`pud_input_setup()`
   提交一次，完成回调里**除 `-ENOENT`/`-ECONNRESET`/`-ESHUTDOWN`（正在卸载 / 总线没了）
   外，无论什么 status 都要重新提交**；旧版 `if (urb->status) return;` 遇到一次抖动就
   永久不再提交，输入就此失灵。回调在中断上下文，重提交用 `GFP_ATOMIC`。
   **不要再为每个样本发 `REQ_EP4_IN` 控制请求**（那是旧协议的做法）。
   注册成什么设备由 `report_mode` 决定（`touch` 默认 / `pointer`，见
   [`notes/architecture.md`](notes/architecture.md) 的"输入设备"一节）：
   **绝对设备必须给分辨率**（`input_abs_set_res()`，用设备上报的 `width_mm/height_mm`），
   否则 libinput 直接判定为驱动 bug。清理顺序：`usb_kill_urb()`（等回调结束）→
   `usb_free_urb()` → `kfree(ep_int_buf)`；`pud->indev` 来自
   `devm_input_allocate_device()`，**不要**再手动 `input_unregister_device()`（会重复注销）。

## 真机测试

- 部署与加载用仓里的 [`scripts/pud-load.sh`](scripts/pud-load.sh)（在**板子上**跑）：
  `load [模块参数...]` / `unload [--stop-gdm]` / `reload` / `status`。它会先校验
  `vermagic` 与 `uname -r` 一致（曾因旧 `.ko` 白折腾一整轮），并在卸载被桌面占住时
  列出占用者、`--stop-gdm` 时按"停 gdm → rmmod → 起 gdm"走一遍。
- **只调触摸就别加载显示**：`scripts/pud-load.sh load input_only=1` 只注册 input 设备，
  没有 DRM/fbdev 节点，桌面会话占不住模块，`unload` 立刻成功 —— 反复试触摸不用停 gdm
  （带显示加载时才需要 `unload --stop-gdm`）。看事件：
  `grep -A5 pud /proc/bus/input/devices` 找 event 号，再
  `sudo timeout 10 cat /dev/input/eventN | od -An -tx2`，按屏幕会看到
  `0003 0000 xxxx`（ABS_X）/ `0003 0001 yyyy`（ABS_Y）/ `0001 014a 0001`（BTN_TOUCH）。
  默认（不带参数）还是显示 + 触摸一起注册。
- **fbdev 编号不固定**：PUD 可能是 `fb0` 也可能是 `fb1`。用
  `cat /sys/class/graphics/fb*/name` 找 `pud-drmdrmfb`，不要写死。
- 要看固件内部状态（解码计数等）走 CMSIS-DAP：OpenOCD 跑在 **Windows 宿主机**，
  WSL 侧用 `gdb-multiarch -q -nh` 连 `localhost:3333`。
  只读检查后要 `monitor resume`，**别用 `monitor reset run`**（会清状态）。
- **不加载驱动也能测全部功能**：用固件仓的 `scripts/`（用户空间 pyusb）。

## 板子上的工作方式（省时间，都是踩过的坑）

1. **一轮只做一件事**：脚本先写好，一次 `scp` 上去跑完 —— 不要在一轮里串多次 ssh、
   gdb、`make modules`。板子一卡，一轮能白等十分钟。
2. **可能挂住的命令一律套 `timeout`**（`lsusb`、`dmesg`、debugfs 读写、`make`、`rmmod`）：
   USB 栈一卡，`lsusb` 会永远不返回，没有 timeout 就整轮坐在工具自己的上限上。
   **工具调用自己的超时压到 ≤4 分钟**，挂住要立刻暴露。
3. **gdb 读固件是 30~60 s 级**：一轮最多读一次，能用 `dmesg` 说清就不读。
4. **驱动只编译一次**：改完一次 `make modules`，`pud.ko` 留在板子上复用。
5. 板子重启后**总线与路径会变**（`6-1` → `3-1`）：脚本里动态发现，别写死接口路径。
6. 下结论前**两侧对账**：主机 `dmesg`/`usbmon` 与设备侧计数器。
7. **EP1 连续失败就先卸载再查** ✗：一次 EP1 stall 足以把主机控制器卡住 ——
   之后 `usb_sg_wait()`/`usb_sg_cancel()` 不返回，DRM modeset 锁被占住，
   `/sys/kernel/debug/dri/*/state` 都读不出来，界面永久黑屏。别靠反复 reload 试探。
   **注**：当前固件对超限的 EP1 数据是**丢弃并重新武装**、不再 stall（2026-09 实测，
   见 [notes/build-and-test.md](notes/build-and-test.md)），正常路径上够不到这个触发点；
   这条纪律保留，是因为老固件与控制器级 stall 仍可能发生。

## 代码约定

- C 风格用**内核风格：tab + 8 宽缩进**（仓库根的 `.clang-format` 取自内核，唯一偏离是
  `UseTab: ForIndentation`，理由写在文件里），`u8/u16/u32` 是内核类型别名。代码已整体按它
  格式化过；**vendored 的 `rgb565_qoi.*` / `rgb565_rle.*` / `jpegenc.*` 不要格式化**
  （要与上游逐字节一致）。
- **注释体不会被 clang-format 修**：`ReflowComments: false` 下 `/*` 之后的续行缩进原样保留，
  格式化对错误缩进是 no-op（`clang-format` 跑前跑后一样）。所以注释续行必须**手写**成
  "每层一个 tab + 一个空格"：`\t * 文本`、`\t */`。这几份源码原本每层 4 空格，
  reformat 只把代码换成 tab、注释留在 tab=4 的对齐上 —— 在 tab=8 的内核风格里看着就是歪的，
  已修过一轮。
- 新增源文件要加进 `Makefile` 的对象列表（写法是
  `$(MODULE_NAME)-y += ...`，其中 `MODULE_NAME:=pud`）。
- 收尾自查：`make modules` 没有新增 warning，`dmesg` 里没有 WARN/oops。
- `make modules` 会顺带生成 **`compile_commands.json`**（用内核的
  `scripts/clang-tools/gen_compile_commands.py` 解析 kbuild 留下的 `.*.cmd`），仓库根的
  `.clangd` 指向它 —— 编辑器/clangd 开箱可用。生成物不入库
  （`.gitignore` 已忽略）；交叉编译器自带的头文件路径若报缺失，用
  `clangd --query-driver=/usr/bin/aarch64-linux-gnu-*` 启动。

## 文档维护

- 知识库在 [`notes/`](notes/README.md)：架构、协议、刷新策略、构建、踩坑。
  改了行为就同步对应文档；协议改动要同时改固件仓的镜像文档。
- **只写已验证的结论**；推测显式标注"未验证"。
- 文档用中文，命令/路径/标识符保留英文。
