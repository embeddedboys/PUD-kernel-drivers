# 显示与刷新策略（DRM 后端）

## 为什么用 DRM simple-pipe

驱动只需要"一块内存 → 一个固定分辨率面板"，没有 planes/CRTC 的复杂需求，
所以用 `drm_simple_display_pipe` + shadow plane：

```c
static const struct drm_simple_display_pipe_funcs pud_display_pipe_funcs = {
    .mode_valid = pud_drm_pipe_mode_valid,
    .enable     = pud_drm_pipe_enable,
    .disable    = pud_drm_pipe_disable,
    .update     = pud_drm_pipe_update,
    DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};
```

支持的格式（`pud_drm_formats[]`）：

| 格式 | 角色 |
| --- | --- |
| `DRM_FORMAT_RGB565` | 面板原生格式，直接 `drm_fb_memcpy` 拷出 |
| `DRM_FORMAT_XRGB8888` | 合成器常用格式，用 `drm_fb_xrgb8888_to_rgb565` 转换 |

默认模式是 `DRM_MODE_INIT(60, 480, 320, 85, 55)`。fbdev 模拟通过 `drm_fbdev_generic_setup(drm, 0)` 建立。

> `pud_drm_pipe_enable()` 只打印一行日志，**不做任何全屏刷新** —— 首帧画面依靠第一次
> atomic commit 走正常 damage 路径完成。改这里时要意识到 `enable` 之后屏幕是黑的，
> 直到第一次 `update`。

## damage 局部刷新的正确性依据

刷新矩形来自：

```c
drm_atomic_helper_damage_merged(old_state, state, &rect)
```

它把所有 damage clip **合并成一个包围盒**（是真实变化区域的**超集**）。用超集是安全的：
多刷不会错，漏刷才会留残影。

无 clip 时怎么办？`drivers/gpu/drm/drm_damage_helper.c:244`（6.1 源码）：

```c
if (!iter->clips || !drm_rect_equals(&state->src, &old_state->src)) {
        iter->clips = NULL;
        iter->num_clips = 0;
        iter->full_update = true;
}
```

也就是说：**没有提供 damage clips，或 plane 的 src 矩形变了（缩放/移动），就退化为整屏刷新**。
这是"局部刷新不会漏"的根本保证，值得记住。

同时驱动显式启用了 damage 上报：

```c
drm_plane_enable_fb_damage_clips(&pud->pipe.plane);
```

并且 framebuffer 用 `drm_gem_fb_create_with_dirty` 创建（`FB_DIRTY` 语义），
意思是"用户空间应当提供 damage"，与上面的 helper 配套。

## 编码：RGB565 QOI / RLE（跟着设备走）

历史包袱：早期用 JPEG（`jpegenc.c`）。JPEGDEC 解码器的 MCU/crop 逻辑在
`x != 0` 的子图上会错位裁切，局部刷新既不准也容易把显示卡死。改成 **QOI** 之后：

- **无损**：不会因为量化产生"越刷越脏"
- **编码极快**：主机侧编码不成为瓶颈
- **解码器按像素处理**：任意子矩形都正确，没有 MCU 对齐问题

**编码器由设备决定**：`PUD_CMD_GET_CAPS` 的 `decoder_type` 是几，就发什么
（`PUD_DECODER_QOI` = 3 → `qoi_encode_rgb565()`，`PUD_DECODER_RLE` = 4 →
`rle_encode_rgb565()`）。给 RLE 固件发 QOI 只会被解码器丢掉（magic 不对），
反过来也一样，所以不要在主机侧写死一种。设备没报能力时按固件的默认值 QOI 走。
目前**只实现了这两种**：LZ4（2）与 JPEG（0/1）在 DRM 局部刷新路径里会明确报错
（`drm_err_once`）并置 `needs_full_refresh`，不会把垃圾推给设备。
（RLE 分支只做了编译验证，固件当前是 `DECODER_TYPE=3`，**未上机验证**。）

两种编码器的最坏情况都是 **3 字节/像素**，所以下面那套分带预算对两者都成立。

### 分带（band splitting）

单次传输的 `size` 字段受 **设备实际上限**约束，而一帧的最坏情况是
**3 字节/像素**。所以按像素数切带：

```c
/* pud->max_band_pixels 由 PUD_CMD_GET_CAPS 在 probe 时问设备得到：
 *   (min(USB_TRANS_MAX_SIZE, caps.frame_max) - PUD_EP1_HEADER_SIZE - 16) / 3
 * 拿不到能力报告时退回 PUD_DEFAULT_BAND_PIXELS (= 21835)。 */

rows = pud->max_band_pixels / (rect->x2 - rect->x1);          /* 每条带的行数 */
if (rows < 1) rows = 1;

for (y = rect->y1; y < rect->y2; y += rows) { ... }
```

**为什么是设备说了算**：同一个数字决定固件侧的 `EP1_RD_BUF_SIZE` 与帧槽大小
（`PUD_MAX_TRANSFER`），而它按板子不同 —— RP2350 是 64 KB，RP2040 只有 256 KB SRAM
所以是 32 KB。写死在驱动里的话，一份模块就不可能同时服务两种板子。
真机日志（RP2350，协议 v2）：

```
pud 7-1:1.0: caps: proto 2, frame_max 65536, decoder 3 -> 21835 pixels per band
```

65536 经 `min(USB_TRANS_MAX_SIZE=65535, …)` 得到 **21835** px（`(65535 - 12 - 16) / 3`）。
v1 的日志里这个数是 **21839**（`(65535 - 16) / 3`）—— 那时窗口矩形走 EP0 控制请求、
12 B EP1 header 不占单次传输的预算，两者别混用。协议细节见
[usb-protocol.md](usb-protocol.md)。

每条带**独立编码、独立 `pud_flush()`**，坐标是真实带边界（`band.y2 - 1` 作为 `ye`）。

> **JPEG 路径只整屏用。** fbdev 后端（`pud_bmp_blit` / fb deferred-IO）发的是整屏 JPEG，
> 坐标固定 `(0,0)`；DRM 后端一律 QOI 分带。不要给 JPEG 帧传非零 `x`：固件侧
> JPEGDEC 在 `x != 0` 时 `iWidthUsed` 会算出负值，`xe` 被填成子图内坐标、`len` 变成巨大的
> 无符号数，一次这样的 flush 就把 `decoder_task` 卡死、帧槽永不释放（真机复现：
> `submitted/drawn = 2/0`，主机持续超时）。固件仓 `notes/decoders.md` 有完整数据。


> 这里曾有一个真实 bug：早期版本无论刷哪条带，`xe/ye` 都填的是整屏右下角，
> 导致 `x > 0` 的窗口刷新位置错误。现已修正。

### `dst_pitch = NULL` 的含义

`pud_buf_copy()` 里所有转换都传 `dst_pitch = NULL`：

```c
drm_fb_memcpy(&dst_map, NULL, src, fb, clip);
drm_fb_xrgb8888_to_rgb565(&dst_map, NULL, src, fb, clip, swap);
```

`NULL` 表示**目标按 clip 宽度紧凑打包** —— 正因为如此，每条带才能从 `pud->tx_buf`
的偏移 0 开始写，而不需要按整屏行宽留 stride。如果哪天要改成"带之间共享缓冲区",
这里必须同步改。

## 缓冲区

在 `pud_drm_dev_init_with_formats()`（`drm.c`）里分配：

| 缓冲区 | 分配方式 | 大小 | 用途 |
| --- | --- | --- | --- |
| `pud->tx_buf` | `vmalloc` | `hdisplay * vdisplay * 2`（480×320 → 307200） | RGB565 转换目标（**只给 CPU 用**，不参与 DMA） |
| `pud->encoder_buf` | `dma_alloc_coherent` | `rgb565_qoi_max_compressed_size(h*v)` = `8 + h*v*3 + 8`（480×320 → 460816） | QOI/RLE 编码输出 + DMA 源 |
| `pud->bulk_sgt` | `sg_alloc_table_from_pages`（页来自 `vmalloc_to_page(encoder_buf)`） | 同上 | **同步** `usb_sg` 路径的 SG 表；`PUD_USB_ASYNC=1` 时**完全不分配**（异步 URB 直接填 `transfer_dma = encoder_dma`） |

`hdisplay`/`vdisplay` 来自 **DRM mode，而 mode 由设备上报的面板参数（`PUD_CMD_GET_CAPS`）
在 probe 时生成**（`pud_mode_init()`），不再是编译期常量 —— 换面板不用改驱动。

**为什么 encoder_buf 必须是 `dma_alloc_coherent` 而不是 `vmalloc`**：
`tx_buf` 只在 CPU 侧读写，`vmalloc` 没问题；但 `encoder_buf` 要被 USB 控制器 DMA 读取，
`vmalloc` 的内存会被直接拒绝（见 [pitfalls.md](pitfalls.md) 的 "rejecting DMA map of vmalloc memory"）。
`dma_alloc_coherent` 返回的虽然是 vmap 地址（CPU 可写），但底层页在 DMA 可达范围内：
**同步**路径再用 `vmalloc_to_page()` 组 SG 表给 `usb_sg_init()`；**异步**路径直接把
`encoder_dma` 填进 URB（`URB_NO_TRANSFER_DMA_MAP`），连 SG 表都不建。

**encoder_buf 必须能装下最坏情况**：`rgb565_qoi_compress()` 在
`output_capacity < max_compressed_size` 时**直接返回 0**（不压缩）。所以按最坏情况申请。

## 失败兜底：`needs_full_refresh`

`pud_flush()` **对调用方是阻塞的**，返回实际字节数或负 errno。阻塞方式由
`PUD_USB_ASYNC` 决定（见 [build-and-test.md](build-and-test.md) 的"构建选项"）：
同步是 `usb_sg_wait()`，异步是 `wait_for_completion_timeout()` + `usb_kill_urb()`。
一旦失败，**这一帧的 damage 就永远丢了** —— 那一块像素会一直保持旧内容，
直到应用恰好重绘它（典型表现就是"残影"）。

所以加了一层兜底：

```c
/* pud_fb_dirty() 内 */
ret = pud_flush(...);
if (ret < 0) {
    pud->needs_full_refresh = true;
    return;
}
```

```c
/* pud_drm_pipe_update() 内 */
if (drm_atomic_helper_damage_merged(old_state, state, &rect) ||
    pud->needs_full_refresh) {
    if (pud->needs_full_refresh) {
        pud->needs_full_refresh = false;
        drm_rect_init(&rect, 0, 0, fb->width, fb->height);
    }
    pud_fb_dirty(...);
}
```

即：**只有在出错时**才整屏重刷，把任何残影的寿命限制在一帧之内。
正常路径零开销（不是周期性全刷，那种做法既慢又没用）。

## 关于残影的一个真实教训

曾经出现过"局部刷新很快，但拖动窗口有残影"的现象。根因**不在**驱动，而在固件：
固件只有 2 个解码帧槽，主机以数百帧/秒推送局部刷新时会**静默丢帧**，
丢掉的那帧里包含某区域的最新内容，随后又被旧内容覆盖回去 → 残影。

修法是在固件侧做 EP1 流控（背压），驱动侧无需改动。详见
`Pico-USB-Display/notes/decoders.md` 的"流控"一节。

**排查经验**：遇到"局部刷新有残影"，按这个顺序怀疑：
1. 固件是否在丢帧（丢帧计数器，见固件 notes）
2. `pud_flush()` 是否失败而 damage 未重试（本节的兜底）
3. damage 覆盖是否完整（`drm_atomic_helper_damage_merged` 的超集语义保证了这点）

## 移植到 6.1 内核时的 API 差异

本驱动同时维护 `kernel-6.12`（上游）和 `rk-6.1.118`（本项目）分支。6.1 与 6.12 的差异：

| 6.12 写法 | 6.1 写法 |
| --- | --- |
| `#include <drm/drm_fbdev_dma.h>` | `#include <drm/drm_fb_helper.h>` |
| `drm_fbdev_dma_setup()` | `drm_fbdev_generic_setup()` |
| `drm_fb_xrgb8888_to_rgb565(..., fmtcnv_state)` | 少一个 `fmtcnv_state` 参数 |
| `drm_shadow_plane_state.fmtcnv_state` | 6.1 无此字段 |
| 若干 pipe 回调宏 | `DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS` |

`pud_drm_alloc()` 用 `devm_drm_dev_alloc()`，失败时返回 `ERR_PTR(-ENOMEM)` ——
调用方**必须**用 `IS_ERR()` 判断而不是 `if (!drm)`，否则会把错误指针当设备用（曾因此 oops）。
