# 显示与刷新策略（DRM 后端）

> 驱动手工搭 plane/CRTC/encoder（simple-KMS 已上游废弃），帧更新走 damage 包围盒 → 分带 →
> 编码 → EP1；flush 失败置 `needs_full_refresh` 兜底整屏，避免残影。

## TL;DR

- **自己搭 KMS 流水线**：`drm_simple_display_pipe` 及其 helper 已废弃、7.3-rc 起从内核移除。
- `drm->mode_config.funcs` 必须在创建 plane/CRTC **之前**设好，否则 oops。
- damage 用 `drm_atomic_helper_damage_merged()` 合成一个包围盒（真实变化的**超集**，多刷安全）；
  无 clip 或 src 变化时 helper 自动退化为整屏。
- `fbdev` 模拟**必须有 shadow buffer**（7.0 只看 `fb->funcs->dirty`），否则用户态写 `/dev/fb0` 被静默忽略。
- 分带大小由设备上报；编码器选择见 [encoders.md](encoders.md)；API 差异见 [kernel-api-differences.md](kernel-api-differences.md)。

## KMS 流水线

驱动只需要"一块内存 → 一个固定分辨率面板"，没有复杂 planes/CRTC 需求。历史上用
`drm_simple_display_pipe` + shadow plane，**现在改成自己搭**（内联版）：

```c
static const struct drm_plane_helper_funcs pud_plane_helper_funcs = {
    .begin_fb_access = drm_gem_begin_shadow_fb_access,
    .end_fb_access   = drm_gem_end_shadow_fb_access,
    .atomic_check    = pud_plane_atomic_check,
    .atomic_update   = pud_plane_atomic_update,
};
```

### 为什么不用 simple-KMS helper

上游 2026-03 的 `drm/simple-kms: Deprecate simple-kms helpers` 把它们正式标记为废弃。技术理由写在
`Documentation/gpu/todo.rst`：`struct drm_simple_display_pipe` 和它的 helper"本意是简化驱动开发，
结果只是在 atomic modesetting 和驱动之间多加了一层中间层"，任务是找到调用者、把
`drm_simple_kms_helper.c` 的 helper **内联进驱动**。**≤ 7.2 还在**，**7.3-rc 起
`drm_simple_kms_helper.c` 没了**（头文件多留一个周期），所以想往新内核走的驱动都得内联。

### 内联了什么

| simple-pipe 内部 | 我们的对应物（`drm.c`） |
| --- | --- |
| `drm_universal_plane_init()` + simple-kms plane funcs | `pud_drm_create_plane()` + `pud_plane_funcs` / `pud_plane_helper_funcs` |
| plane `atomic_check`（`DRM_PLANE_NO_SCALING` 等参数） | `pud_plane_atomic_check()`（同参数：不缩放、不挪位、必须铺满） |
| plane `atomic_update` → `pipe->funcs->update` | `pud_plane_atomic_update()`（damage 合并 → 分带 → `pud_fb_dirty()`） |
| `DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS` | `drm_gem_begin/end_shadow_fb_access` + `drm_gem_reset/duplicate/destroy_shadow_plane_state` |
| `drm_crtc_init_with_planes()` + simple-kms CRTC funcs | `pud_drm_create_crtc()` + `pud_crtc_funcs` / `pud_crtc_helper_funcs` |
| CRTC `atomic_check` | `pud_crtc_atomic_check()`（逐字相同：CRTC 只有一个 plane，得让它进每一次提交） |
| CRTC `atomic_enable/disable` → `pipe->funcs->enable/disable` | `pud_crtc_atomic_enable/disable`（仍只打印一行） |
| `drm_simple_encoder_init()` + `drm_connector_attach_encoder()` | `pud_drm_create_encoder()` + `pud_encoder_funcs` |

顺序上的坑：**`drm->mode_config.funcs` 必须在创建 plane/CRTC 之前设好**，对象创建时会拿它校验。
参考仓库 `linux-drm-tutorial` 留了一条 `RIP: drm_mode_validate_driver` 的 oops 记录，就是设晚了。

参考实现：同工作区 `linux-drm-tutorial`（手工搭的最小 KMS 驱动，可 `make qemu` 里 `insmod` 看回调顺序）。

移植验证（2026-09，7.0.0-34 客户机 + 真设备直通）：probe/caps/DRM 注册/fbcon 正常，
写 `/dev/fb0` 64 KB → EP1 31 笔 / 95966 字节（与 simple-pipe 版几乎逐字节一致），`rmmod` 干净，
`initial_mode=1` 也照常点亮（`pud_crtc_atomic_enable` → "initial mode set on the panel"）。

### 支持的格式

| 格式 | 角色 |
| --- | --- |
| `DRM_FORMAT_RGB565` | 面板原生格式，直接 `drm_fb_memcpy` 拷出 |
| `DRM_FORMAT_XRGB8888` | 合成器常用格式，用 `drm_fb_xrgb8888_to_rgb565` 转换 |

`drm_fb_memcpy()` 默认分支**不认** `DRM_FORMAT_RGB565`，所以 `pud_buf_copy()` 必须显式处理它，
否则报 `Format is not supported: RG16 little-endian`。

默认模式由**设备上报的面板参数**生成（`pud_mode_init()` →
`DRM_MODE_INIT(60, xres, yres, width_mm ?: 85, height_mm ?: 55)`），拿不到能力报告时退回
`pud_default_display`（480×320、74×49 mm）—— `85/55` 只是 mm 为 0 时的兜底；实测（设备报 70×40 mm）
生成的是 480×320 / **70×40**。fbdev 模拟通过 `drm_fbdev_generic_setup(drm, 0)` 建立。

### fbdev 模拟必须有 shadow buffer

否则用户态写 `/dev/fb0` 会被**静默忽略**。这块屏不是扫描输出：像素只有靠一次提交走 flush 路径才能出去。

7.0 只剩一条判据：**`fb->funcs->dirty` 有没有值**。`drm_fbdev_dma_driver_fbdev_probe()` 据此二选一
—— 有 dirty 走 shadowed 那一支（`vzalloc` 系统内存 + deferred IO，用户态 mmap 写完由
`drm_fb_helper_deferred_io` 变成 damage 交给我们），没有就把 GEM buffer 直接映射给用户态，mmap 写完
**没人知道**。`drm_gem_fb_create_with_dirty` 提供的正是这个回调（我们的 `.fb_create` 就是它）；
`prefer_shadow` 只是给用户态看的提示，fbdev 客户端根本不读它。6.1 那套 `drm_fbdev_use_shadow_fb()`
（`prefer_shadow_fbdev` / `prefer_shadow` / `fb->funcs->dirty` 三选一）在 7.0 已不存在。

> 实测（2026-09，6.1）：没有 shadow 时往 `/dev/fb0` 画整屏，usbmon 里 EP1 **一笔都没有**
> （同一窗口里 fbcon 光标和合成器刷新都正常 —— 它们分别走 fb 层显式标脏和 DRM，所以只有 fbdev
> 用户态程序暴露这个坑）；设上 `prefer_shadow_fbdev = true` 后**同一个脚本变成 4368 笔 / 2.3 MB**。
> 实测（2026-09，7.0，QEMU + 真设备直通）：写 `/dev/fb0` 64 KB → EP1 7 笔 / 95780 B。
> 代价：一块整屏 shadow（480×320×2 = 300 KB）+ 每次 damage 一次拷贝。

> `pud_drm_pipe_enable()` 只打印一行日志，**不做任何全屏刷新** —— 首帧靠第一次 atomic commit 走正常
> damage 路径完成。改这里要意识到：`enable` 之后屏幕是黑的，直到第一次 `update`。

## damage 局部刷新的正确性依据

刷新矩形来自 `drm_atomic_helper_damage_merged(old_state, state, &rect)`：它把所有 damage clip
**合并成一个包围盒**（真实变化区域的**超集**）。用超集是安全的：多刷不会错，漏刷才会留残影。

无 clip 时，`drivers/gpu/drm/drm_damage_helper.c`（6.1）：

```c
if (!iter->clips || !drm_rect_equals(&state->src, &old_state->src)) {
        iter->clips = NULL;
        iter->num_clips = 0;
        iter->full_update = true;
}
```

即：**没有 damage clips，或 plane 的 src 矩形变了（缩放/移动），就退化为整屏刷新**。
这是"局部刷新不会漏"的根本保证。

驱动同时显式启用 damage 上报（`drm_plane_enable_fb_damage_clips(&pud->pipe.plane)`），并用
`drm_gem_fb_create_with_dirty` 创建 framebuffer（`FB_DIRTY` 语义 = "用户空间应当提供 damage"）。

## 分带（band splitting）

单次传输的 `size` 受**设备实际上限**约束，而一帧最坏情况是 **3 字节/像素**（deflate 只会更小，
QOI/RLE 都是这个上界），所以按像素数切带：

```c
/* pud->max_band_pixels 由 PUD_CMD_GET_CAPS 在 probe 时问设备得到：
 *   (min(USB_TRANS_MAX_SIZE, caps.frame_max) - PUD_EP1_HEADER_SIZE - 16) / 3
 * 拿不到能力报告时退回 PUD_DEFAULT_BAND_PIXELS (= 21835)。 */

rows = pud->max_band_pixels / (rect->x2 - rect->x1);          /* 每条带的行数 */
if (rows < 1) rows = 1;

for (y = rect->y1; y < rect->y2; y += rows) { ... }
```

**为什么是设备说了算**：同一个数字决定固件侧的 `EP1_RD_BUF_SIZE` 与帧槽大小（`PUD_MAX_TRANSFER`），
而它按板子不同 —— RP2350 是 64 KB，RP2040 只有 256 KB SRAM 所以是 32 KB。写死在驱动里的话，
一份模块就不可能同时服务两种板子。真机日志（RP2350，协议 v2）：

```
pud 7-1:1.0: caps: proto 2, frame_max 65536, decoder 3 -> 21835 pixels per band
```

65536 经 `min(USB_TRANS_MAX_SIZE=65535, …)` 得到 **21835** px（`(65535 - 12 - 16) / 3`）。
v1 日志里这个数是 **21839**（`(65535 - 16) / 3`）—— 那时窗口矩形走 EP0 控制请求、12 B EP1 header
不占单次传输预算，两者别混用。协议细节见 [usb-protocol.md](usb-protocol.md)。

每条带**独立编码、独立 `pud_flush()`**，坐标是真实带边界（`band.y2 - 1` 作为 `ye`）。

> 这里曾有一个真实 bug：早期版本无论刷哪条带，`xe/ye` 都填整屏右下角，导致 `x > 0` 的窗口刷新
> 位置错误。现已修正。

### `dst_pitch = NULL` 的含义

`pud_buf_copy()` 里所有转换都传 `dst_pitch = NULL`：

```c
drm_fb_memcpy(&dst_map, NULL, src, fb, clip);
drm_fb_xrgb8888_to_rgb565(&dst_map, NULL, src, fb, clip, swap);
```

`NULL` 表示**目标按 clip 宽度紧凑打包** —— 正因为如此，每条带才能从 `pud->tx_buf` 的偏移 0 开始写，
而不需要按整屏行宽留 stride。如果哪天要改成"带之间共享缓冲区"，这里必须同步改。

## 缓冲区

在 `pud_drm_dev_init_with_formats()`（`drm.c`）里分配：

| 缓冲区 | 分配方式 | 大小 | 用途 |
| --- | --- | --- | --- |
| `pud->tx_buf` | `vmalloc` | `hdisplay * vdisplay * 2`（480×320 → 307200） | RGB565 转换目标（**只给 CPU 用**，不参与 DMA） |
| `pud->encoder_buf` | `dma_alloc_coherent` | `rgb565_qoi_max_compressed_size(h*v)` = `8 + h*v*3 + 8`（480×320 → 460816） | QOI/RLE 编码输出 + DMA 源 |
| `pud->bulk_sgt` | `sg_alloc_table_from_pages`（页来自 `vmalloc_to_page(encoder_buf)`） | 同上 | **同步** `usb_sg` 路径的 SG 表；`PUD_USB_ASYNC=1` 时**完全不分配** |

`hdisplay`/`vdisplay` 来自 **DRM mode，而 mode 由设备上报的面板参数在 probe 时生成**，不再是编译期常量。

**为什么 `encoder_buf` 必须是 `dma_alloc_coherent` 而不是 `vmalloc`**：`tx_buf` 只在 CPU 侧读写，
`vmalloc` 没问题；但 `encoder_buf` 要被 USB 控制器 DMA 读取，`vmalloc` 会被直接拒绝
（见 [pitfalls.md](pitfalls.md) 的 "rejecting DMA map of vmalloc memory"）。`dma_alloc_coherent`
返回的虽是 vmap 地址（CPU 可写），底层页在 DMA 可达范围内：**同步**路径再用 `vmalloc_to_page()`
组 SG 表给 `usb_sg_init()`；**异步**路径直接把 `encoder_dma` 填进 URB
（`URB_NO_TRANSFER_DMA_MAP`），连 SG 表都不建。

**`encoder_buf` 必须能装下最坏情况**：`rgb565_qoi_compress()` 在
`output_capacity < max_compressed_size` 时**直接返回 0**（不压缩），所以按最坏情况申请。

## 失败兜底：`needs_full_refresh`

`pud_flush()` **对调用方是阻塞的**，返回实际字节数或负 errno。阻塞方式由 `PUD_USB_ASYNC` 决定
（见 [build-and-test.md](build-and-test.md)）：同步是 `usb_sg_wait()`，异步是
`wait_for_completion_timeout()` + `usb_kill_urb()`。一旦失败，**这一帧的 damage 就永远丢了**
—— 那块像素保持旧内容，直到应用恰好重绘它（典型表现就是"残影"）。

失败还会**限速**（`usb.c`）：连续失败到 `PUD_FLUSH_FAIL_MAX`（= 3）次后，最多每秒再试一次
（`flush_fails` / `flush_last_fail`）。理由是一次失败传输会占住 USB worker 整个看门狗周期；
每秒一次仍能在设备回来时自动接上。失败本身打 `EP1 transfer failed (…)`（`dev_warn_ratelimited`），
第 3 次补一条 `dev_err` —— 所以**dmesg 干净 + 有 EP1 流量**（见 [usbmon.md](usbmon.md)）才等于
"真的没出错"。

兜底：

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

即：**只有在出错时**才整屏重刷，把残影寿命限制在一帧内。正常路径零开销。

## 残影的一个真实教训

曾出现"局部刷新很快，但拖动窗口有残影"。根因**不在**驱动，而在固件：只有 2 个解码帧槽，
主机以数百帧/秒推送局部刷新时会**静默丢帧**，丢掉的那帧含某区域最新内容，随后又被旧内容覆盖回去
→ 残影。修法是在固件侧做 EP1 流控（背压），驱动侧无需改动。详见固件仓
`Pico-USB-Display/notes/decoders.md` 的"流控"一节。

**排查顺序**（遇到"局部刷新有残影"）：

1. 固件是否在丢帧（丢帧计数器，见固件 notes）
2. `pud_flush()` 是否失败而 damage 未重试（上面的兜底）
3. damage 覆盖是否完整（`drm_atomic_helper_damage_merged` 的超集语义保证这点）

## 相关

- [encoders.md](encoders.md)：编码器选择、QOI/RLE/QOI+deflate、JPEG 路径限制（含 `x != 0` 的坑）。
- [kernel-api-differences.md](kernel-api-differences.md)：6.1/6.12/7.0 API 差异。
- [usb-protocol.md](usb-protocol.md)：EP1 header、`frame_max`、流控。
