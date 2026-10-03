# 架构与代码地图

> `pud` 是 USB 显示驱动：主机把画面编码成压缩流经 EP1 发出，Pico 固件解码后刷 SPI/I8080 TFT；
> 反向由 EP4 主动推送触摸。设备通过 `PUD_CMD_GET_CAPS` 上报自己的面板与传输参数，驱动不写死分辨率。

## TL;DR

- 后端默认 DRM（`PUD_DEF_DISP_BACKEND = PUD_DISP_BACKEND_DRM`），fbdev 是旧路径。
- 主路径：atomic commit → damage 包围盒 → 分带 → RGB565 → 编码 → `pud_flush()` → EP1 bulk。
- **面板参数与分带上限全部来自设备**（`PUD_CMD_GET_CAPS`），拿不到才退回默认值。
- 触摸在设备侧可选（`PUD_CAPS_TOUCH`）；没有能力位就不注册输入设备。
- 协议见 [usb-protocol.md](usb-protocol.md)；刷新策略见 [display-and-refresh.md](display-and-refresh.md)；
  触摸见 [input-touch.md](input-touch.md)；踩坑见 [pitfalls.md](pitfalls.md)。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `usb.c` | 驱动主干：厂商协议收发、能力查询（`pud_query_caps()`）、`pud_flush()`、probe/disconnect、后端选择 |
| `pud.h` | `struct pud` 主结构、端点/请求常量、`struct pud_ep1_header`、`struct pud_caps` |
| `drm.c` | DRM 后端：手工搭 plane/CRTC/encoder、damage 局部刷新、按设备解码器选编码器并分带 |
| `fb.c` | fbdev 后端（`PUD_DISP_BACKEND_FBDEV`）：老式 framebuffer 接口 |
| `encoder.c` / `.h` | 编码层封装：`qoi_encode_rgb565()` / `rle_encode_rgb565()` / QOI+deflate |
| `rgb565_qoi.c` / `.h` | RGB565 QOI 编解码库（来自 `rgb565-qoi/`，仅加 `__KERNEL__` include 适配） |
| `rgb565_rle.c` / `.h` | RGB565 RLE 编解码库（来自 `rgb565-rle/`，同样只改 include） |
| `jpegenc.c` / `.h` | JPEG 编码路径，由 DRM decoder 1 和旧 fbdev 后端复用，整屏、坐标固定 `(0,0)` |
| `input.c` | 触摸输入：EP4 中断 URB（设备主动推送）+ `input_dev` 注册 |
| `dma_gem_dma_helper.c` | 内核 `drm_gem_dma_helper` 的 vendored 副本：**不在 Makefile、没被编译**（DRM 后端用内核自带 `drm_gem_dma_*`），改它没有任何效果 |

## 构建组成

改这里才能把新文件编进去（`MODULE_NAME:=pud`）：

```make
obj-m += pud.o
pud-y += usb.o jpegenc.o encoder.o rgb565_qoi.o rgb565_rle.o fb.o drm.o input.o
```

## 编译期开关（`pud.h` / `Makefile`）

| 开关 | 默认 | 说明 |
| --- | --- | --- |
| `PUD_DEF_DISP_BACKEND` | `PUD_DISP_BACKEND_DRM` | 后端选择：`0`=fbdev，`1`=DRM |
| `PUD_ENABLE_INPUT_SUPPORT` | `1` | 是否注册触摸 input 设备 |
| `PUD_USB_ASYNC` | `0` | EP1 传输路径：`0`=同步 `usb_sg`，`1`=异步 URB。见 [build-and-test.md](build-and-test.md) |

## 运行期参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `input_only` | `0` | `insmod pud.ko input_only=1` 时**只注册触摸**，不注册 DRM/fbdev 节点。调触摸时用它：没有显示节点，桌面会话占不住模块，`rmmod` 立刻成功。**不只是测试脚手架**：面板可只当输入设备用（配 `report_mode=pointer`），不要提议拆掉 |
| `report_mode` | `touch` | 输入设备注册成 `touch` 或 `pointer`；差别见 [input-touch.md](input-touch.md) |
| `initial_mode` | `0` | 从 probe 直接提交一次固定 mode，让**没有 userspace** 时面板也点亮（`drm.c:pud_drm_set_initial_mode()`）。默认关有原因：它把驱动放进"没人在环里"的发送路径，只有固件**丢弃不可信 header 而不是 stall EP1** 之后才安全。固件 2026-09 起满足该条件，`initial_mode=1` 已真机验证。**用完别带着它卸载**（见 [pitfalls.md](pitfalls.md) 4.5） |

## 设备参数（不写死在驱动里）

`pud_probe()` 在注册任何东西之前先问设备（`PUD_CMD_GET_CAPS`）：

- **传输上限 / 解码器** → `pud->frame_max`、`pud->max_band_pixels`、`pud->decoder_type`；
- **面板参数** → 每台设备自己的 `pud->display_data`（`pud->display` 指向它）：分辨率、旋转、bpp、
  总线时钟、触摸轮询周期。

顺序有原因：`pud_query_caps()` 用一个临时堆对象当 DMA 缓冲（`ctrl_buf` 不能是栈），查询结果再交给
`pud_drm_alloc()` / `pud_framebuffer_alloc()` —— DRM mode、`encoder_buf` 大小、fbdev 显存、输入设备
轴范围全由它推出来。**不要退回写死的 480×320**：换面板时那是唯一会静默错的地方。老固件只回
16 字节时 `pud_apply_caps()` 保留 `pud_default_display` 并打 `old firmware: no panel parameters`。

字段定义与校验规则见 [usb-protocol.md](usb-protocol.md) 的 `PUD_CMD_GET_CAPS` 一节（含 magic 校验、
`max_band_pixels` 公式、RP2350/RP2040 差异）。**已验证（2026-09，真机）**：

- 新固件（32 B 应答）→ `caps: proto 2, frame_max 65536, decoder 3 -> 21835 pixels per band` +
  `panel: 480x320, rotation 1, 16 bpp, 50000 kHz, interface 0, 70x40 mm, touch yes`；
  默认加载路径 `pud-drm: mode: 480x320`、connector `connected enabled`、modes = `480x320`
  （**由设备上报的 xres/yres 生成**）；
- 老固件（16 B 应答）→ `(old firmware: no panel parameters)` + 默认值，加载/卸载干净、无 WARN/oops；
- `input_only=1` 只注册输入设备（dmesg 无任何 DRM 行），`rmmod` 连续三轮全部成功；
- **未上机验证**：RLE 编码路径（固件当前 `DECODER_TYPE=3`）——`pud_encode_band()` 的 RLE 分支只做了编译验证。

## 数据流（DRM 后端，当前主路径）

```
应用/合成器 (gnome-shell, X, ...)
   │  atomic commit，带 FB_DAMAGE_CLIPS
   ▼
pud_plane_atomic_update()                drm.c
   │  drm_atomic_helper_damage_merged() → 本次变化的包围盒
   ▼
pud_fb_dirty()                       drm.c
   │  JPEG 扩成整屏；无损编码按 max_band_pixels 分带
   ▼
pud_send_band()                      drm.c
   │  pud_buf_copy() → pud_encode_band() → pud_flush()
   │  QOID 字典只在发送成功后提交
   ▼
pud_flush()                          usb.c
   │  pud_prepare_frame() 校验容量、组头并补齐奇数载荷
   │  EP1 bulk：12 B header (xs,ys,xe,ye,size) + 压缩载荷（v2：无控制请求）
   │  路径由 PUD_USB_ASYNC 选：0 = usb_sg_init()+usb_sg_wait()；1 = usb_submit_urb()+completion
   ▼
Pico 固件：解码 → TFT
```

反方向（触摸，`input.c`）：`EP4 中断 IN 端点 → pud_tp_urb_callback() → input_report_*()`。

## 设备识别

```c
/* usb.c:pud_ids[] —— 只匹配接口 0（图像接口） */
{ USB_DEVICE_INTERFACE_NUMBER(0x2E8A, 0x0001, 0) },
```

**必须限定接口号**：`USB_DEVICE()` 会匹配设备的每一个接口，而固件从 2026-09 起有第二个接口
（picoboot reset），否则 probe 两次、注册出第二块永远 enable 不起来的 DRM card。接口 0 从第一版
固件起就是图像接口，老设备不受影响（详见 [pitfalls.md](pitfalls.md) 2.4）。

`MODULE_DEVICE_TABLE(usb, pud_ids)` 让 udev 能自动加载。固件上报的 `SerialNumber` 是固定串
（`usb.c` 通过 `REQ_EP2_IN`/`PUD_CMD_GET_SN` 读到的 8 字节板子唯一 ID，仅打印到 dmesg）。

## 与固件的对应关系

每处协议字段都能在固件找到镜像实现，修改时必须**成对改**：

| 驱动 | 固件 |
| --- | --- |
| `pud.h` 的 `struct pud_ep1_header`（写在 `encoder_buf` 最前面） | `include/pud.h` 的 `struct pud_ep1_header` |
| `pud_transfer()` 的 EP2 请求头（4 字节） | `usbd_vendor.c` 的 `struct req_ep2_in` |
| `REQ_*` / `TYPE_VENDOR`（`pud.h`） | `src/cherryusb/usbd_vendor.h` |
| `USB_TRANS_MAX_SIZE` | `DECODER_FRAME_MAX` / `EP1_RD_BUF_SIZE` |
| 编码器 `qoi_encode_rgb565()` | 解码器 `qoi_drawimg()` |

协议细节见 [usb-protocol.md](usb-protocol.md)。
