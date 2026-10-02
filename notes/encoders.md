# 编码器：QOI / RLE / QOI+deflate

> 主机用哪个编码器由设备 `PUD_CMD_GET_CAPS` 的 `decoder_type` 决定，不在主机侧写死；
> 三种编码器最坏情况都是 3 字节/像素，所以同一套分带预算对它们都成立。

## TL;DR

- `decoder_type` → 编码器：`3` QOI、`4` RLE、`5` QOI + raw deflate；`0/1/2`（tjpgd/JPEGDEC/LZ4）
  在 DRM 局部刷新路径里**明确报错**并置 `needs_full_refresh`，不把垃圾推给设备。
- 设备没报能力时按固件的默认值 **QOI** 走。
- **给 RLE 固件发 QOI 只会被解码器丢掉**（magic 不对），反之亦然。
- JPEG 只用于 fbdev 整屏路径：**不要给 JPEG 帧传非零 `x`**（固件 JPGEODEC 裁切会卡死解码任务）。
- `PUD_DECODER_RLE`（4）在真机上**未验证**，只做了编译验证。

## 编码器选择

`pud_encode_band()` 按 `pud->decoder_type` 分派：

| `decoder_type` | 编码器 | 状态 |
| --- | --- | --- |
| `PUD_DECODER_QOI` = 3 | `qoi_encode_rgb565()` | 固件当前默认，真机验证 |
| `PUD_DECODER_RLE` = 4 | `rle_encode_rgb565()` | **未上机验证**（固件当前 `DECODER_TYPE=3`） |
| `PUD_DECODER_QOIZ` = 5 | QOI → 内核 raw deflate | QEMU 客户机验证，未在 RK3588 真机验证 |
| `PUD_DECODER_TJPGD` = 0 / `JPEGDEC` = 1 / `LZ4` = 2 | — | DRM 路径报错并置 `needs_full_refresh` |

为什么从 JPEG 改成 QOI：JPEGDEC 解码器的 MCU/crop 逻辑在 `x != 0` 的子图上会错位裁切，
局部刷新既不准也容易把显示卡死。QOI 之后：**无损**（不会越刷越脏）、**编码极快**（主机不成为瓶颈）、
**解码器按像素处理**（任意子矩形都正确，没有 MCU 对齐问题）。

（JPEG 路径保留在 `jpegenc.c`，只被 fbdev 后端 `pud_fb_deferred_io()` → `jpeg_encode_rgb565()`
使用：整屏、坐标固定 `(0,0)`。详见文末"JPEG 路径的限制"。）

## QOI + deflate（`PUD_DECODER_QOIZ` = 5）

固件侧的实验编码，目前只做进 RP2350 的构型。主机把每个 band 先 QOI 编码，再对那串 QOI 码流跑一次
**内核自带的 raw deflate**（`lib/zlib_deflate` 的 `zlib_deflateInit2()` / `zlib_deflate()`，
`EXPORT_SYMBOL` 导出）—— deflate 这一层不用 vendor 任何编码器，这点和 LZ4 一样（QOI/RLE 两份仍要 vendor）。
设备把它 inflate 回 QOI 码流再解码，所以 EP1 帧格式、分带规则、流控都不变。

参数见 `encoder.c` 顶部注释：`level 1`、`windowBits -12`（负数 = 不要 zlib 头与 adler32，设备要的就是
raw）、`memLevel 6`。相对纯 QOI 的字节数：合成桌面 **−29%**、照片壁纸 **−25%**；窗口放到 32 KB 只再多
0.6%，级别升到 6 只再多 1.5%，所以选了省内存的一档。

内存：`qoiz_buf`（vmalloc，一个 band 的 QOI 最坏值
`rgb565_qoi_max_compressed_size(pud->max_band_pixels)` ≈ 64 KB）、deflate workspace（vzalloc，
约 48 KB）和一个 `z_stream_s`。两者都只给 CPU 用，不是 DMA buffer。

生命周期：在 `pud_drm_setup_encoder()` 里分配，**必须等能力报告落地之后**（`pud_drm_alloc()`
时还不知道 `decoder_type`）；分配失败会让 probe 失败，**不回落到 QOI** —— 那种固件对每个传输都做
inflate，给它发 QOI 等于每帧都被丢掉（设备侧 `g_decoder_stat_qoiz_bad` 会一直涨）。释放挂在
`pud_drm_release_buffers()` 上（正常拔插走 `pud_drm_unregister()`）。

状态（2026-09-30）：**只在 QEMU 客户机里验证过**（真设备直通，7.0.0-34 客户机）—— 315 帧提交全部
drawn、`dropped`/`oversize` 为 0、线上字节数是纯 QOI 的 0.74 倍、客户机 dmesg 无 WARN/oops、
`rmmod` 干净。**没有**在 RK3588 真机上验证，也**没有**接过真实合成器的 damage 流。客户机里另有
77 帧被设备判为坏数据，来自第一次 probe（那次 `GET_CAPS` 在模拟 xHCI 上超时，驱动退回默认的 QOI
编码），重新加载之后就没再出现。

## JPEG 路径的限制

fbdev 后端发的是整屏 JPEG，坐标固定 `(0,0)`；DRM 后端按 `decoder_type` 选 QOI/RLE 分带。
**不要给 JPEG 帧传非零 `x`**：固件侧 JPEGDEC 在 `x != 0` 时 `iWidthUsed` 会算出负值，`xe` 被填成
子图内坐标、`len` 变成巨大的无符号数，一次这样的 flush 就把 `decoder_task` 卡死、帧槽永不释放
（真机复现：`submitted/drawn = 2/0`，主机持续超时）。固件仓 `Pico-USB-Display/notes/decoders.md`
有完整数据。

## 相关

- [display-and-refresh.md](display-and-refresh.md)：分带规则、缓冲区大小、`needs_full_refresh`。
- [usb-protocol.md](usb-protocol.md)：`decoder_type` 字段定义与 `PUD_DECODER_*` 取值。
