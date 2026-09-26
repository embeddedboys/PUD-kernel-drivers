# 显示与刷新策略（DRM 后端）

## 流水线：自己搭 plane + CRTC + encoder

驱动只需要"一块内存 → 一个固定分辨率面板"，没有复杂的 planes/CRTC 需求。历史上这是用
`drm_simple_display_pipe` + shadow plane 写的，**现在改成自己搭**（内联版）：

```c
static const struct drm_plane_helper_funcs pud_plane_helper_funcs = {
    .begin_fb_access = drm_gem_begin_shadow_fb_access,
    .end_fb_access   = drm_gem_end_shadow_fb_access,
    .atomic_check    = pud_plane_atomic_check,
    .atomic_update   = pud_plane_atomic_update,
};
```

### 为什么不用 simple-KMS helper

上游 2026-03 的 `drm/simple-kms: Deprecate simple-kms helpers`（Thomas Zimmermann）把它们
正式标记为废弃。理由分两层，都在那条提交里：

- **提交信息**：这些 helper"已经被弃用好几年了"，多数驱动都迁走了，但**仍然时不时收到基于
  它们写的新驱动**；标记 deprecated 就是为了止住这个趋势，同时给剩下的驱动留 TODO
  （难度标 Easy，方便新贡献者上手）。
- **它加进 `Documentation/gpu/todo.rst` 的 TODO**（技术理由）：`struct drm_simple_display_pipe`
  和它的 helper"本意是简化驱动开发，结果只是**在 atomic modesetting 和驱动之间多加了一层
  中间层**"。任务写得很具体：找到调 `drm_simple_display_pipe_init()` 的驱动，**把
  `drm_simple_kms_helper.c` 里的 helper 内联进驱动**、按驱动自己的命名重写，
  "such that no simple-KMS interfaces are required"；`drm_simple_encoder_init()` 同理。

时间线：**≤ 7.2 还在**（7.2 里 `.c` 完好，只是文档被删、头文件顶上加了"不要在新代码里用"），
**7.3-rc 起 `drm_simple_kms_helper.c` 没了**（头文件留一个周期）。所以想继续往新内核走的驱动
都得内联 —— 我们做的就是这个。

### 内联了什么：simple-pipe 内部 → 我们

| simple-pipe 内部 | 我们的对应物（`drm.c`） |
| --- | --- |
| `drm_universal_plane_init()` + `drm_simple_kms_plane_funcs/helper_funcs` | `pud_drm_create_plane()` + `pud_plane_funcs` / `pud_plane_helper_funcs` |
| plane `atomic_check`：`drm_atomic_helper_check_plane_state(…, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, false, false)` | `pud_plane_atomic_check()`（同参数：不缩放、不挪位、必须铺满） |
| plane `atomic_update` → `pipe->funcs->update(pipe, old_state)` | `pud_plane_atomic_update()`（内容没变：damage 合并 → 分带 → `pud_fb_dirty()`） |
| `DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS` | `drm_gem_begin/end_shadow_fb_access` + `drm_gem_reset/duplicate/destroy_shadow_plane_state` |
| `drm_crtc_init_with_planes()` + `drm_simple_kms_crtc_funcs/helper_funcs` | `pud_drm_create_crtc()` + `pud_crtc_funcs` / `pud_crtc_helper_funcs` |
| CRTC `atomic_check`：`drm_atomic_helper_check_crtc_primary_plane()` + `drm_atomic_add_affected_planes()` | `pud_crtc_atomic_check()`（逐字相同：CRTC 只有一个 plane，得让它进每一次提交） |
| CRTC `atomic_enable/disable` → `pipe->funcs->enable/disable` | `pud_crtc_atomic_enable/disable`（仍然只打印一行） |
| `drm_simple_encoder_init()`（`DRM_MODE_ENCODER_NONE`）+ `drm_connector_attach_encoder()` | `pud_drm_create_encoder()` + `pud_encoder_funcs` |

一个顺序上的坑：**`drm->mode_config.funcs` 必须在创建 plane/CRTC 之前设好**，对象创建时会
拿它校验。参考仓库 `linux-drm-tutorial` 的 `drm.c` 里留了一条 `RIP: drm_mode_validate_driver`
的 oops 记录，就是设晚了。

参考实现：同工作区的 `linux-drm-tutorial`（它的 `drm.c` 就是手工搭的最小 KMS 驱动，带完整虚拟
链路，可以在 `make qemu` 的客户机里 `insmod` 跑起来看回调顺序）。

移植验证（2026-09，7.0.0-34 客户机 + 真设备直通）：probe/caps/DRM 注册/fbcon 正常，
写 `/dev/fb0` 64 KB → EP1 31 笔 / 95966 字节（与 simple-pipe 版几乎逐字节一致），
`rmmod` 干净，`initial_mode=1` 也照常点亮（`pud_crtc_atomic_enable` → "initial mode set on
the panel"）。

支持的格式（`pud_drm_formats[]`）：

| 格式 | 角色 |
| --- | --- |
| `DRM_FORMAT_RGB565` | 面板原生格式，直接 `drm_fb_memcpy` 拷出 |
| `DRM_FORMAT_XRGB8888` | 合成器常用格式，用 `drm_fb_xrgb8888_to_rgb565` 转换 |

默认模式由**设备上报的面板参数**生成（`pud_mode_init()` →
`DRM_MODE_INIT(60, xres, yres, width_mm ?: 85, height_mm ?: 55)`），拿不到能力报告时退回
`pud_default_display`（480×320、74×49 mm）—— 所以 `85/55` 只是 mm 为 0 时的兜底，实测
（设备报 70×40 mm）生成的是 480×320 / **70×40**。fbdev 模拟通过
`drm_fbdev_generic_setup(drm, 0)` 建立。

> **fbdev 模拟必须有 shadow buffer**，否则用户态写 `/dev/fb0` 会被**静默忽略**。这块屏不是
> 扫描输出：像素只有靠一次提交走我们的 flush 路径才能出去。
>
> 7.0 只剩一条判据：**`fb->funcs->dirty` 有没有值**。`drm_fbdev_dma_driver_fbdev_probe()`
> 据此二选一 —— 有 dirty 走 shadowed 那一支（`vzalloc` 一块系统内存 + deferred IO，用户态
> mmap 写完由 `drm_fb_helper_deferred_io` 变成 damage 交给我们），没有就把 GEM buffer
> 直接映射给用户态，mmap 写完**没人知道**。`drm_gem_fb_create_with_dirty` 提供的正是这个
> 回调（我们的 `.fb_create` 就是它）；`prefer_shadow` 只是给用户态看的提示，fbdev 客户端
> 根本不读它。6.1 那套 `drm_fbdev_use_shadow_fb()`（`prefer_shadow_fbdev` / `prefer_shadow`
> / `fb->funcs->dirty` 三选一）在 7.0 已经不存在。
>
> 实测（2026-09，6.1）：没有 shadow 时往 `/dev/fb0` 画整屏，usbmon 里 EP1 **一笔都没有**
> （同一窗口里 fbcon 的光标和合成器的刷新都正常在发 —— 它们分别走 fb 层的显式标脏和 DRM，
> 所以控制台与桌面看不出问题，这个坑只在 fbdev 用户态程序上暴露）；
> 设上 `drm->mode_config.prefer_shadow_fbdev = true` 之后，**同一个脚本变成 4368 笔 / 2.3 MB**
> （脚本按行写，所以一行一次更新）。
> 实测（2026-09，7.0，QEMU + 真设备直通）：写 `/dev/fb0` 64 KB → EP1 7 笔 / 95780 B。
>
> 代价：一块整屏 shadow（480×320×2 = 300 KB）+ 每次 damage 一次拷贝。

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

> **JPEG 路径只整屏用。** fbdev 后端（`fb.c` 的 `pud_fb_deferred_io()` →
> `jpeg_encode_rgb565()`）发的是整屏 JPEG，坐标固定 `(0,0)`；DRM 后端按设备上报的
> `decoder_type` 选编码器（QOI 或 RLE）分带。不要给 JPEG 帧传非零 `x`：固件侧
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

失败还会**限速**（`usb.c`）：连续失败到 `PUD_FLUSH_FAIL_MAX`（= 3）次之后，最多每秒再试
一次（`flush_fails` / `flush_last_fail`）。理由是一次失败的传输会占住 USB worker 整个
看门狗周期，设备不在时 damage 来得多快就打得多快；每秒一次仍然能在设备回来时自动接上。
失败本身打 `EP1 transfer failed (…)`（`dev_warn_ratelimited`），第 3 次补一条 `dev_err`
—— 所以**dmesg 干净 + 有 EP1 流量**（见 [usbmon.md](usbmon.md)）才等于"真的没出错"。

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

## 各内核分支的 API 差异

本驱动同时维护 `kernel-6.12`（上游）、`rk-6.1.172`（板子）和 `7.0.0-34-generic`（本机
generic 内核，名字跟运行内核走）分支。

**流水线不一样**：`kernel-6.12` 和 `rk-6.1.172` 用的还是 `drm_simple_display_pipe`
（它们的内核里它还完好），本机这支是上面那套内联版 —— 往前走到 7.3+ 的就是后者。

### 6.12 与 6.1

| 6.12 写法 | 6.1 写法 |
| --- | --- |
| `#include <drm/drm_fbdev_dma.h>` | `#include <drm/drm_fb_helper.h>` |
| `drm_fbdev_dma_setup()` | `drm_fbdev_generic_setup()` |
| `drm_fb_xrgb8888_to_rgb565(..., fmtcnv_state)` | 少一个 `fmtcnv_state` 参数 |
| `drm_shadow_plane_state.fmtcnv_state` | 6.1 无此字段 |
| 若干 pipe 回调宏 | `DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS` |

### 6.12 与 7.0

| 6.12 写法 | 7.0 写法 |
| --- | --- |
| `drm_fbdev_dma_setup(drm, 0)` | `drm_client_setup(drm, NULL)` **加上**驱动里的 `DRM_FBDEV_DMA_DRIVER_OPS`（即 `.fbdev_probe`） |
| `from_timer()` | `timer_container_of()` |
| `destroy_timer_on_stack()` | `timer_destroy_on_stack()` |
| `drm_dbg()` | `drm_dbg_driver()`（`drm_dbg` 在 7.0 只是它的别名） |
| `struct drm_driver.date` | 字段已删除 |
| `mode_config.prefer_shadow_fbdev` | 字段不存在；fbdev shadow 只认 `fb->funcs->dirty`（见上） |

第一条最容易漏，而且**编得过、加载也不报错**：7.0 的 `drm_client_setup()` 只注册客户端，
分配 fbdev 后备存储的是驱动自己的 `.fbdev_probe` —— `drm_fb_helper_single_fb_probe()` 里
第一句就是 `if (drm_WARN_ON(dev, !dev->driver->fbdev_probe)) return -EINVAL;`。
少了它，dmesg 里只有一条 WARN，**没有 `/dev/fb0`、没有 fbcon**。

`pud_drm_alloc()` 用 `devm_drm_dev_alloc()`，失败时返回 `ERR_PTR(-ENOMEM)` ——
调用方**必须**用 `IS_ERR()` 判断而不是 `if (!drm)`，否则会把错误指针当设备用（曾因此 oops）。
