# 架构与代码地图

## 定位

`pud` 是一个 **USB 显示驱动**：一块 Raspberry Pi Pico（RP2350）通过 USB 接入 Linux 主机，
把主机送过来的压缩图像流解码后刷到一块 SPI/I8080 TFT 上；反向还有一个（目前打桩的）触摸通道。

主机侧（本仓库）负责：抓取显示内容 → 编码成压缩流 → 通过批量端点发送 → 注册成一个 DRM/fbdev 显示设备。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `usb.c` | 驱动主干：USB 厂商协议收发、能力查询（`pud_query_caps()`）、`pud_flush()`、probe/disconnect、DRM/fbdev 后端选择 |
| `pud.h` | `struct pud` 主结构、端点/请求常量、后端选择开关 |
| `drm.c` | DRM 后端：`drm_simple_display_pipe` 注册、damage 局部刷新、按设备解码器选编码器并分带 |
| `fb.c` | fbdev 后端（`PUD_DISP_BACKEND_FBDEV`）：老式 framebuffer 接口 |
| `encoder.c` / `encoder.h` | 编码层封装，对外暴露 `qoi_encode_rgb565()` / `rle_encode_rgb565()` |
| `rgb565_qoi.c` / `rgb565_qoi.h` | RGB565 QOI 编解码库（来自 `rgb565-qoi/`，头文件加了 `__KERNEL__` 适配） |
| `rgb565_rle.c` / `rgb565_rle.h` | RGB565 RLE 编解码库（来自 `rgb565-rle/`，同样只改 include 适配） |
| `jpegenc.c` / `jpegenc.h` | 早期 JPEG 编码路径，现在仅在 fbdev 后端的 `pud_bmp_blit()` 里还被用到 |
| `input.c` | 触摸输入：EP4 中断 URB（设备主动推送）+ `input_dev` 注册 |
| `dma_gem_dma_helper.c` | GEM DMA helper（DRM 后端用） |

## 构建组成

`Makefile` 里的模块组成（改这里才能把新文件编进去）：

```make
obj-m += pud.o
pud-y += usb.o jpegenc.o encoder.o rgb565_qoi.o rgb565_rle.o fb.o drm.o input.o
```

## 编译期开关（`pud.h`）

| 宏 | 默认 | 说明 |
| --- | --- | --- |
| `PUD_DEF_DISP_BACKEND` | `PUD_DISP_BACKEND_DRM` | 后端选择：`0`=fbdev，`1`=DRM |
| `PUD_ENABLE_INPUT_SUPPORT` | `1` | 是否注册触摸 input 设备 |

## 运行期参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `input_only` | `0` | `insmod pud.ko input_only=1` 时**只注册触摸**，不注册 DRM/fbdev 节点。调触摸时用它：没有显示节点，桌面会话就不会把模块占住，`rmmod` 能立刻卸掉、反复加载。 |

## 输入设备（EP4）

触摸在设备侧是**可选**的（`PUD_CAPS_TOUCH`）：多数板级配置没有控制器，那种固件
既不置该位、`tp_polling_period` 也报 0，驱动就**不注册输入设备**（日志
`device reports no touch controller, not registering input`）。只有能力位为 1 时
`pud_input_setup()` 才会跑。

`report_mode` 决定注册成哪种设备 —— 两种都是绝对设备，区别在桌面怎么用它们：

| 模式 | 事件集 | libinput 判定 | 桌面表现 |
| --- | --- | --- | --- |
| `touch`（默认） | `BTN_TOUCH` + `ABS_X/Y` + **MT-B**（`ABS_MT_SLOT/POSITION_X/Y/TRACKING_ID`）+ `INPUT_PROP_DIRECT` + 分辨率 | `Capabilities: touch` | 摸面板 = 点面板上那个位置；但**绑到哪个输出由合成器决定**，GNOME 对外接触摸屏没有可配的映射（mutter#2080/#3480），绑错屏时只能改用 `pointer` 模式或把它变成主输出 |
| `pointer` | `BTN_LEFT/BTN_RIGHT` + `ABS_X/Y` + 分辨率，**不置** `DIRECT`、不报 `BTN_TOUCH`、不建 MT | `Capabilities: pointer` | 绝对指针（就是 QEMU `usb-tablet` 那类），坐标映射到**整个虚拟桌面**：一定能用，代价是"摸面板"不等于"点面板" |

两条硬要求（都来自 [libinput 的绝对轴文档](https://wayland.freedesktop.org/libinput/doc/latest/absolute-axes.html)）：

1. **绝对设备必须给分辨率**，否则 libinput 直接认为这是驱动 bug。驱动用设备上报的
   `width_mm/height_mm` 算 units/mm 并 `input_abs_set_res()`（`absinfo.resolution` 是整数，
   所以 libinput 反推出的尺寸会差几个百分点：74×49 mm 的面板被报成 80×46 mm）。
   拿不到尺寸就不设，并打一条告警。
2. 触摸屏用 `INPUT_PROP_DIRECT` 判定；单点屏 libinput 也支持，但 MT-B 才是标准形态
   （手势/长按/拖拽的 tracking 靠 `ABS_MT_TRACKING_ID`，松手必须发 `-1`）。

**实测（2026-09，真机 + 板子上的 libinput 1.25.0）**：`touch` 模式 libinput 报
`Capabilities: touch`、`Size: 80x46mm`，8 次拖动共 1000+ 个 MT 点、tracking id 递增、
每次松手 `-1`、`BTN_TOUCH` 各 8 次按下/松开、采样间隔中位 **8.0 ms**；
`pointer` 模式 libinput 报 `Capabilities: pointer`，一次 5.0 秒拖动 432 个坐标点
（x 26..372、y 76..270）、只有 **2 个 `BTN_LEFT`**（按下 + 松手）、
没有 `BTN_TOUCH`/`ABS_MT_*`，间隔同样中位 **8.0 ms**。

> `pointer` 模式多出一个已知副作用：没有 `INPUT_PROP_DIRECT` 时内核的 `joydev`
> 也会绑上这个设备（`/proc/bus/input/devices` 里多一个 `js0`）。不影响桌面把它当指针用，
> 只是会多一个摇杆节点。

## 设备参数（不写死在驱动里）

`pud_probe()` 在注册任何东西之前先问设备（`PUD_CMD_GET_CAPS`）：

- **传输上限 / 解码器** → `pud->frame_max`、`pud->max_band_pixels`、`pud->decoder_type`；
- **面板参数** → 每台设备自己的 `pud->display_data`（`pud->display` 指向它）：
  分辨率、旋转、bpp、总线时钟、触摸轮询周期。

顺序是有原因的：`pud_query_caps()` 用一个临时堆对象当 DMA 缓冲（`ctrl_buf` 不能是栈），
查询结果再交给 `pud_drm_alloc()` / `pud_framebuffer_alloc()` —— DRM mode、`encoder_buf`
大小、fbdev 显存、输入设备的轴范围全都由它推出来。**不要退回写死的 480×320**：
换一块面板（或换 RP2040 那套配置）时那是唯一会静默错的地方。老固件只回 16 字节时，
`pud_apply_caps()` 保留 `pud_default_display` 并打一条 `old firmware: no panel parameters`。

**已验证（2026-09，真机）**：

- 新固件（28 B 应答）→
  `caps: proto 2, frame_max 65536, decoder 3 -> 21835 pixels per band` +
  `panel: 480x320, rotation 1, 16 bpp, 50000 kHz, interface 0, touch poll 10 ms`；
  默认加载路径 `pud-drm: mode: 480x320`、`/sys/class/drm/card3-USB-1` = `connected enabled`、
  modes = `480x320`（**由设备上报的 xres/yres 生成**）；
- 老固件（16 B 应答，烧写前那一版）→ `(old firmware: no panel parameters)` + 默认值，
  加载/卸载干净、无 WARN/oops；
- `input_only=1` 只注册输入设备（dmesg 里没有任何 DRM 行），`rmmod` 连续三轮全部成功。
- **未上机验证**：RLE 编码路径（固件当前 `DECODER_TYPE=3`）——`pud_encode_band()` 的
  RLE 分支只做了编译验证。

## 数据流（DRM 后端，当前主路径）

```
应用/合成器 (gnome-shell, X, ...)
   │  atomic commit，带 FB_DAMAGE_CLIPS
   ▼
pud_drm_pipe_update()                drm.c
   │  drm_atomic_helper_damage_merged() → 得到本次变化的包围盒
   ▼
pud_fb_dirty()                       drm.c
   │  ① 按行分带（每带 ≤ pud->max_band_pixels 像素，值由设备上报）
   │  ② pud_buf_copy() 把该带转成 RGB565，放进 pud->tx_buf
   │  ③ qoi_encode_rgb565() 编码进 pud->encoder_buf
   ▼
pud_flush()                          usb.c
   │  ① EP1 bulk：12 B header (xs,ys,xe,ye,size) + 压缩载荷（v2：不再有控制请求）
   │  ② usb_sg_init() + usb_sg_wait() 从 EP1 批量发出压缩流
   ▼
Pico 固件：解码 → TFT
```

反方向（触摸，`input.c`）：

```
EP4 中断 IN 端点 → pud_tp_urb_callback() → input_report_*()
```

## 设备识别

```c
static struct usb_device_id pud_ids[] = {
    { USB_DEVICE(0x2E8A, 0x0001) },   /* Raspberry Pi 的 VID + 自定义 PID */
};
```

`MODULE_DEVICE_TABLE(usb, pud_ids)` 让 udev 能自动加载。固件上报的 `SerialNumber` 是固定串
（`usb.c` 里通过 `REQ_EP2_IN`/`PUD_CMD_GET_SN` 读到的 8 字节板子唯一 ID，仅打印到 dmesg 用）。

## 与固件的对应关系

驱动这边每处协议字段都能在固件找到镜像实现，修改时需要**成对改**：

| 驱动 | 固件 |
| --- | --- |
| `pud.h` 的 `struct pud_ep1_header`（写在 `encoder_buf` 最前面） | `include/pud.h` 的 `struct pud_ep1_header` |
| `pud_transfer()` 的 EP2 请求头（4 字节） | `usbd_vendor.c` 的 `struct req_ep2_in` |
| `REQ_*` / `TYPE_VENDOR`（`pud.h`） | `src/cherryusb/usbd_vendor.h` |
| `USB_TRANS_MAX_SIZE` | `DECODER_FRAME_MAX` / `EP1_RD_BUF_SIZE` |
| 编码器 `qoi_encode_rgb565()` | 解码器 `qoi_drawimg()` |

协议细节见 [usb-protocol.md](usb-protocol.md)。
