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
| `PUD_DECODER_QOID` = 6 | QOI → `tinyc`（固定 Huffman + 预设字典） | **策略 A**：序号乐观、不做 `GET_QOID` 查询；编码器宿主机闭环验证，未上机 |
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

## QOI + deflate + 跨帧字典（`PUD_DECODER_QOID` = 6）

band 先 QOI 编码，再对那串 QOI 跑一次 raw deflate，**并把该矩形上一次发出去的 QOI 码流当作预设
字典**，前面加一个 16 字节子头（`struct pud_qoid_header`：magic / flags / reserved / dict_serial /
dict_len）。设备用它自己的 `tinyd` 解码，字典放在每个帧槽的窗口里。

**内核自带的 zlib 做不了这一层**，两个硬约束都缺（已核实 `include/linux/zlib.h`）：

- **没有 `Z_FIXED`**：设备侧的 `tinyd` 只解 stored 与**固定 Huffman** 块，动态 Huffman 直接报
  `TINYD_ERR_FORMAT`。内核只提供 `Z_DEFAULT_STRATEGY`/`Z_FILTERED`/`Z_HUFFMAN_ONLY`，无法强制固定块。
- **没有 `deflateSetDictionary`**：而跨帧字典正是这个编解码器的全部意义。

所以自带 [`tinyc.c`](../tinyc.c)/[`tinyc.h`](../tinyc.h) —— 固件侧 `tinyd` 的**编码镜像**（同一套
RFC 1951 表），只用固定 Huffman 与 stored 块、支持预设历史，两条路径取更小的那条。scratch
（`tinyc_work_size()` ≈ **128 KB**，哈希链）与字典历史一起 `vzalloc`。

### 序号与字典的对应关系（策略 A）

设备给**每一笔被接受的条带**分配一个自增 serial，并记住哪个窗口持有它的 QOI；delta 用
`dict_serial` 指名。驱动这边：

- 自己记最近 `PUD_QOID_HISTORY`（= 3）条**同矩形**的 QOI 码流，delta 取其中 serial 最新的一条；
  没有候选（或换矩形）就发**键帧**（`dict_serial = 0`）。
- serial 由驱动自己数，**为真正 flush 成功的条带才消耗**（`pud_qoid_commit()`）。给一个设备没收到的
  条带消耗序号，会让之后**每一笔** delta 都指错——而驱动看不到设备的计数器，会一路静默拒收。
- 乐观之处：假定设备的计数与驱动一致。固件不变量 2 的 EP1 流控保证 `dropped == 0`，所以正常路径成立。
  一旦漂移，代价是**那一笔不被绘制**（不是花屏），下一轮键帧即重新同步。
- 环形缓冲**只在 flush 成功后写**：槽位元数据在提交前还描述着上一条带，提前写字节会让"元数据指向
  A、字节是 B"，而设备会**照解不误**——那是花屏而不是拒收。

### 分带预算受**窗口半宽**约束，不只是传输上限（真机踩过）

`max_band_pixels` 原来只按传输上限算（`(frame_max-12-16)/3` = 21835），这对 6 是**错的**：设备把每条带
解进 `win[dict_len .. dict_len + PUD_DELTA_WIN/2)`，所以**本条带的 QOI 流本身**也必须 ≤ 16384 B。
超了设备整条丢弃并计 `g_decoder_stat_qoid_oversize`——**驱动看不到**，那块面板就静默停止更新。

真机现象：**kiosk 正常、切到 GNOME 后整块黑**。原因是 cage 只报小块 damage（每条带 QOI 都小于半窗，
`qoid_oversize == 0`），而 Xorg 首次提交是**整屏 480×320**，QOI 远超 16384 → 每条都被丢。
计数器上是 `submitted/drawn` 各 80、`qoid_oversize=7`。

修法（`usb.c`，`decoder_type == 6` 时再收一次）：

```c
u32 fit = (PUD_QOID_DICT_MAX - 16) / 3;   /* 16384 B → 5456 px，QOI 最坏 3 B/px + 16 B 框架 */
if (pud->max_band_pixels > fit)
        pud->max_band_pixels = fit;
```

`qoid_pack()` 另加 `qoi_len > PUD_QOID_DICT_MAX → -E2BIG`，让这类错误在**驱动侧可见**，而不是被设备
静默丢掉。修后真机实测：强制一次 modeset 的 30 条带**全部绘制、`qoid_oversize` 零新增**。

代价：整屏刷新时带数 8 → 30。要省回来可以改成**自适应**——先按大带编码，超半窗再对半拆重编，
只有复杂内容才拆；现在用的是保守的固定收窄。

已知缺口：设备的 `PUD_DELTA_WIN` 与 `DECODER_FRAME_SLOTS` **都不在 `PUD_CMD_GET_CAPS` 里**，驱动按
RP2350 的 32768/3 写死（`PUD_QOID_DELTA_WIN`/`PUD_QOID_HISTORY`）。RP2040 构型的窗口只有一半，
字典超长会被设备拒（`g_decoder_stat_qoid_oversize`，主机看不到）。要正经支持两块板，得让它上报，
或改用 `PUD_CMD_GET_QOID`（一次约 2.9 ms，见固件仓 `notes/qoid.md`）。

状态（2026-10-02）：编码器在宿主机闭环验证——3000 次随机（随机字典 × 随机数据 × 4 种熵、ASan/UBSan）
经 `tinyd` 全部还原；无匹配可用时与 zlib `Z_FIXED` 逐字节一致；端到端载荷模拟通过，同一条带键帧
514 B、对上一次的 delta 270 B。
**已上机（RK3588，内核 6.1.172）**：kiosk（cage/Wayland）连续渲染 4490 条带，`qoid_bad == 0`
（全部码流被设备解出）、`dropped == 0`、`qoid_mismatch` 稳定在 4（启动瞬态）、`qoid_oversize == 0`。
**未验证**：GNOME 会话。另外注意 **GNOME 跑 X11 时不会往这块面板呈现**（Xorg 配好了输出却不提交帧），
Wayland 会话才走 cage 那样的 atomic + damage 路径。

## JPEG 路径的限制

fbdev 后端发的是整屏 JPEG，坐标固定 `(0,0)`；DRM 后端按 `decoder_type` 选 QOI/RLE 分带。
**不要给 JPEG 帧传非零 `x`**：固件侧 JPEGDEC 在 `x != 0` 时 `iWidthUsed` 会算出负值，`xe` 被填成
子图内坐标、`len` 变成巨大的无符号数，一次这样的 flush 就把 `decoder_task` 卡死、帧槽永不释放
（真机复现：`submitted/drawn = 2/0`，主机持续超时）。固件仓 `Pico-USB-Display/notes/decoders.md`
有完整数据。

## 相关

- [display-and-refresh.md](display-and-refresh.md)：分带规则、缓冲区大小、`needs_full_refresh`。
- [usb-protocol.md](usb-protocol.md)：`decoder_type` 字段定义与 `PUD_DECODER_*` 取值。
