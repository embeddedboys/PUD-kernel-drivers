# 架构与代码地图

## 定位

`pud` 是一个 **USB 显示驱动**：一块 Raspberry Pi Pico（RP2350）通过 USB 接入 Linux 主机，
把主机送过来的压缩图像流解码后刷到一块 SPI/I8080 TFT 上；反向还有一个（目前打桩的）触摸通道。

主机侧（本仓库）负责：抓取显示内容 → 编码成压缩流 → 通过批量端点发送 → 注册成一个 DRM/fbdev 显示设备。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `usb.c` | 驱动主干：USB 厂商协议收发、`pud_flush()`、probe/disconnect、DRM/fbdev 后端选择 |
| `pud.h` | `struct pud` 主结构、端点/请求常量、后端选择开关 |
| `drm.c` | DRM 后端：`drm_simple_display_pipe` 注册、damage 局部刷新、QOI 编码与分带 |
| `fb.c` | fbdev 后端（`PUD_DISP_BACKEND_FBDEV`）：老式 framebuffer 接口 |
| `encoder.c` / `encoder.h` | 编码层封装，对外暴露 `qoi_encode_rgb565()` |
| `rgb565_qoi.c` / `rgb565_qoi.h` | RGB565 QOI 编解码库（来自 `rgb565-qoi/`，头文件加了 `__KERNEL__` 适配） |
| `jpegenc.c` / `jpegenc.h` | 早期 JPEG 编码路径，现在仅在 fbdev 后端的 `pud_bmp_blit()` 里还被用到 |
| `input.c` | 触摸输入：中断端点 URB + `input_dev` 注册（当前固件未真正上报数据） |
| `dma_gem_dma_helper.c` | GEM DMA helper（DRM 后端用） |

## 构建组成

`Makefile` 里的模块组成（改这里才能把新文件编进去）：

```make
obj-m += pud.o
pud-y += usb.o jpegenc.o encoder.o rgb565_qoi.o fb.o drm.o input.o
```

## 编译期开关（`pud.h`）

| 宏 | 默认 | 说明 |
| --- | --- | --- |
| `PUD_DEF_DISP_BACKEND` | `PUD_DISP_BACKEND_DRM` | 后端选择：`0`=fbdev，`1`=DRM |
| `PUD_ENABLE_INPUT_SUPPORT` | `1` | 是否注册触摸 input 设备 |

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
   │  ① EP0 控制请求 REQ_EP1_OUT，告知 (xs,ys,xe,ye,size)
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
| `usb.c` 的 `struct req_ep1_out` / `pud_feed_ctrl_buf()` | `src/cherryusb/usbd_vendor.c` 的 `struct req_ep1_out` |
| `pud_transfer()` 的 EP2 请求头（4 字节） | `usbd_vendor.c` 的 `struct req_ep2_in` |
| `REQ_*` / `TYPE_VENDOR`（`pud.h`） | `src/cherryusb/usbd_vendor.h` |
| `USB_TRANS_MAX_SIZE` | `DECODER_FRAME_MAX` / `EP1_RD_BUF_SIZE` |
| 编码器 `qoi_encode_rgb565()` | 解码器 `qoi_drawimg()` |

协议细节见 [usb-protocol.md](usb-protocol.md)。
