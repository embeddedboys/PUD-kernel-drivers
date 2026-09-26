# USB 厂商协议（权威定义）

> 这是**主机（驱动）↔ 设备（Pico 固件）之间唯一的接口约定**。
> 固件侧的镜像说明见 `Pico-USB-Display/notes/usb-protocol.md`；两边必须保持一致。

## 枚举信息

| 项 | 值 | 来源 |
| --- | --- | --- |
| VID:PID | `0x2E8A:0x0001` | `usb.c` 的 `pud_ids[]`；固件 `usbd_vendor.h` 的 `VENDOR_ID`/`PRODUCT_ID` |
| 接口类 | `0xFF`（vendor specific），子类/协议 `0` | 固件 `config_descriptor[]` |
| 端点数量 | 3 | 同上 |
| 供电 | bus powered，`USBD_MAX_POWER 500` | 同上 |
| 速率 | **Full-Speed**（批量端点 MPS = 64） | 板上 `dmesg` 实测："new full-speed USB device" |

> 注意：驱动侧并没有把 Pico 当成高速设备。若要改高速，`USB_TRANS_MAX_SIZE`、分带尺寸、
> 固件缓冲尺寸都要重新评估。

## 端点分配

| 端点 | 方向/类型 | 用途 | 状态 |
| --- | --- | --- | --- |
| EP0 | control | 厂商请求（查询命令；**窗口协商在 v2 已删除**） | 使用中 |
| EP1 | OUT / bulk | **图像压缩流**（QOI 帧数据） | 使用中 |
| EP2 | IN / bulk | 查询响应（如读序列号） | 使用中 |
| EP3 | OUT / bulk | — | 固件已定义 `REQ_EP3_OUT`/`EP3_OUT_ADDR`，但**未写进配置描述符**，未实现 |
| EP4 | IN / interrupt (64B, bInterval 8) | 触摸数据（设备主动推送） | 见下 |

## EP4 触摸上报

设备**主动推送**：主机的 interrupt IN URB 常挂（不要每个样本发一次控制请求），
触摸任务每 10 ms 轮询一次控制器，按下期间每帧一次、松手补一帧，空闲不发。
**轮询周期与 `bInterval` 要一起改**：主机只在 `bInterval` 到点时才来取报告，只调一个没用
（早期 33 ms + 33 ms 那一版的实测见下面的"实测采样率"）。

| 字节 | 含义 |
| --- | --- |
| 0 | flags，bit0 = 按下 |
| 1–2 | x >> 8、x & 0xff（大端） |
| 3–4 | y >> 8、y & 0xff |
| 5 | sequence（回绕） |
| 6 | version = 1 |
| 7 | 保留 |

- 坐标是**显示坐标系**下的面板坐标（0..479 / 0..319），设备已按 `TFT_ROTATION` 变换并
  钳位，驱动直接用（`ABS_X/ABS_Y` 的 480/320 范围就是它）。
- 字节 0..4 与旧驱动解析的布局一致；version != 0 表示这版固件真的支持触摸。
- `REQ_EP4_IN`(0x05) 仍可用：它返回**当前**这一帧（轮询式主机）。驱动**不用**它 —— 那是旧做法，
  会把设备的推送节奏拖成每个样本一次控制传输（驱动里现在只剩 `pud.h` 的宏定义，没有调用点）。
- 空闲时设备不发帧，所以 `usb_submit_urb()` 会一直挂着 —— 这是正常的，
  **不要在 URB 回调里因为 `-ETIMEDOUT`/`-EPROTO` 就停止重提交**。
- **实测采样率**（2026-09，真机拖动）：固件 33 ms 轮询 + 33 ms `bInterval` 时相邻坐标点
  间隔 32 ms；改成 10 ms + 8 ms 后中位 **8.0 ms（≈125 Hz）**（一次 7.5 s 拖动 563 个点）。
  驱动不用改：`usb_fill_int_urb()` 用的是描述符里的 `bInterval`。
- 驱动侧实现（`input.c`）：`pud_input_setup()` 只提交一次 URB，回调里除
  `-ENOENT`/`-ECONNRESET`/`-ESHUTDOWN` 外都重新提交；`version != 1` 的报告不解析
  （0 = 这版固件没有触摸），长度不足 8 字节的短报告只告警。上报用
  `BTN_TOUCH`/`BTN_LEFT` + `ABS_X`/`ABS_Y`（单点，不走 MT slot），并置
  `INPUT_PROP_DIRECT` 让上层知道这是触摸屏。
- 只测触摸时用 `insmod pud.ko input_only=1`（不注册 DRM/fbdev，`rmmod` 随时能卸），
  见 [architecture.md](architecture.md)。

## 请求号

固件 `usbd_vendor.h` 与驱动 `pud.h` 中的定义完全一致：

```
REQ_EP0_OUT = 0x00      REQ_EP0_IN  = 0x01
REQ_EP1_OUT = 0x02  <-  **v2 起废弃**（仅保留编号，固件会对它 stall）
REQ_EP2_IN  = 0x03      REQ_EP3_OUT = 0x04      REQ_EP4_IN = 0x05
```

驱动的 `bmRequestType` 固定为 `TYPE_VENDOR | USB_DIR_OUT` = `0x40`（厂商请求、主机→设备、设备接收者）。

> 驱动里 `REQ_EP0_OUT` / `REQ_EP0_IN` 只是照抄固件定义，**没有任何调用点**（死代码）。

## 数据通道 1：EP1 图像帧

### 时序（协议 v2）

**一次 EP1 批量传输搞定**，矩形和长度都在数据最前面的 12 字节里，没有控制请求：

```
EP1 bulk OUT  [ struct pud_ep1_header | payload ]
   同步 usb_sg_init/usb_sg_wait 或 异步 usb_submit_urb + completion
   （构建时由 PUD_USB_ASYNC 选，见 notes/build-and-test.md）
```

（v1 是"EP0 窗口协商 + EP1 数据"两次传输。实测那个控制请求每次 ~0.14 ms，全速总线上
还可能撞上一整帧；小矩形连发时省掉它值 **+46%**（32×32 连发 2516 → 3768 rect/s）。）

### EP1 header 载荷

驱动 `usb.c` 的 `struct pud_ep1_header`（小端，和固件 `include/pud.h` 一致）：

```c
struct pud_ep1_header {
    u16 xs;     /* 起始列（含） */
    u16 ys;     /* 起始行（含） */
    u16 xe;     /* 结束列（含！不是排他） */
    u16 ye;     /* 结束行（含！） */
    u32 size;   /* 随后载荷的字节数 */
};
```

- `sizeof(...)` = **12** 字节（无填充）。
- 驱动把它写在 `pud->encoder_buf` 的**最前面**，编码结果紧跟在后面（`encoder_buf_size`
  的可用量因此少 12 B），一次 SG 传输把两段一起发出去 —— 不用额外缓冲、也不违反
  "传输 buffer 必须 DMA 可映射"的约束。
- `xe`/`ye` 是**闭区间**：驱动传来的是 `band.x2 - 1` / `band.y2 - 1`。
  固件据此算出 `width = xe - xs + 1`。
- 坐标是**相对于整屏**的绝对坐标。
- 固件**两段读**：先一个最大包（64 B，header 一定在里面），再按 `size` 读剩下的。
  所以传输结束不依赖短包。
- 一个**不可信**的 header（`12 + size` 超过设备缓冲，或矩形越界）会被
  固件**丢弃并重新武装**，不是 stall（`g_ep1_stat.oversize++` / `.bad++`）。这是设备侧
  刻意为之：stall 会让主机的写失败，而"主机 `clear_halt` 后重试"实测会把宿主控制器
  卡死到连板子都重启不干净（见固件 `src/cherryusb/usb.c` 里
  `usbd_vendor_ep1_bulk_out()` 的注释）。正常驱动靠分带不会送出这种 header。

### `size` 的奇偶：不要求，但主机最好仍然取偶

驱动 `pud_flush()` 会把 `data_size` 向上取到偶数：

```c
/* kept for firmware built before 2026-09, which rejected an odd size */
if (data_size % 2)
    data_size += 1;
```

**设备已经不再要求偶数**（2026-09 实测：RP2350 上 4 奇 4 偶、987~43271 B 全部落地，
`got == total`）。老固件会拒绝奇数 `size`：丢弃这一笔、计进 `g_ep1_stat.oversize`，
**宿主看不到任何错误** —— 所以取偶这一行留着，代价是每笔多 1 字节，换来对老固件的兼容。
`scripts/pud_usb.py` 与 LVGL 模板同理。

取偶之后 header 里的 `size` 可能比真实 QOI 长度大 1，固件会多收到一个尾随字节；QOI 解码
读到结束标记（`00..01`）即停，多余字节不会被消费。**不要依赖"size 精确等于编码长度"。**

### 传输上限

| 常量 | 值 | 位置 | 含义 |
| --- | --- | --- | --- |
| `USB_TRANS_MAX_SIZE` | 65535 | 驱动 `pud.h` | 主机侧单次传输上限（含 12 B header） |
| `PUD_DEFAULT_BAND_PIXELS` | `(65535-12-16)/3` = 21835 | 驱动 `pud.h` | 拿不到能力报告时的兜底单带像素数 |
| `pud->max_band_pixels` | 21835（RP2350） | 驱动 `drm.c` | **实际使用的**单带上限，来自 `PUD_CMD_GET_CAPS` |
| `PUD_MAX_TRANSFER` | 65536 / 32768 | 固件 `usbd_vendor.h` | **按板子**定的单次传输与帧槽上限（RP2350 / RP2040） |
| `DECODER_FRAME_SLOTS` | 2 | 固件 `decoder.c` | 帧槽数量（双缓冲，尺寸 = `PUD_MAX_TRANSFER`） |

**关键约束**：`12 + 一帧的压缩结果` 必须 ≤ **设备上报的** `frame_max`。v2 里固件是在
读到 header 之后才校验的，超限的那一笔会被**丢弃**（`g_ep1_stat.oversize++`，宿主看不到
错误），既不是静默截断、也不是 stall。驱动靠分带（见
[display-and-refresh.md](display-and-refresh.md)）保证这一点。

> 实测（2026-09，固件 `b88c42…`）：用临时超限补丁故意声明 1 MiB 的 payload 后，宿主
> **没有**拿到任何传输错误，固件侧 `g_ep1_stat.oversize` 增长——即走的是丢弃分支。
> `usb_clear_halt()` 那条路径在当前固件上够不到。

### 流控（很重要）

固件**不会**无条件接收 EP1 数据。当解码帧槽全忙时它**故意不武装 EP1**，主机的批量
传输因此阻塞等待，直到解码任务腾出槽位才续上（v2 里这个判断直接发生在武装 EP1 时，
不再有"控制请求先被挡住"那一步）。

对驱动而言这是透明的：一次传输只是多等一会儿。但要注意：

- `pud_flush()` 里有个 **3 秒看门狗**，两条路径实现不同：同步走栈上 timer →
  `usb_sg_cancel()`，异步走 `wait_for_completion_timeout()` → `usb_kill_urb()`
  （见 [build-and-test.md](build-and-test.md) 的 `PUD_USB_ASYNC`）。
  只要固件解码正常（毫秒级），就不会触发。
- 一旦真的超时，这一帧的 damage 就永久丢失（`pud_fb_dirty()` 曾静默忽略返回值的坑，
  现在会置 `needs_full_refresh` 兜底，见 [display-and-refresh.md](display-and-refresh.md)）。

## 数据通道 2：EP2 查询

驱动 `pud_transfer()`：

```c
pud->ctrl_buf[0] = cmd & 0xff;
pud->ctrl_buf[1] = (cmd >> 8) & 0xff;
pud->ctrl_buf[2] = len & 0xff;
pud->ctrl_buf[3] = (len >> 8) & 0xff;
usb_control_msg(..., REQ_EP2_IN, TYPE_VENDOR | USB_DIR_OUT, ..., pud->ctrl_buf, 16, ...);
/* 随后 */
usb_bulk_msg(udev, usb_rcvbulkpipe(udev, EP2_IN_ADDR), data, len, &actual_length, ...);
```

固件侧对应的结构是 `struct req_ep2_in { u16 cmd; u16 size; }` ——
即**前 4 字节**是 `cmd`（小端）+ `size`（小端）。注意这里也用 16 字节的 wLength 传 4 字节有效数据。

### 已实现的命令

| cmd | 名称 | 响应 |
| --- | --- | --- |
| `0x01` | `PUD_CMD_GET_SN` | 8 字节板子唯一 ID，写入 `ep2_write_buffer` 后从 EP2 IN 发回 |
| `0x02` | `PUD_CMD_GET_CAPS` | 16 字节 `struct pud_caps`（magic / proto_ver / frame_max / decoder_type） |
| 其他 | — | 固件回**零长度包**（主机读回短包 → 判定不支持） |

驱动侧调用点：`pud_read_unique_id()` 与 `pud_read_caps()`，都由 `pud_probe()` 在注册
显示设备之后调用。

### `PUD_CMD_GET_CAPS`：设备上报自己的传输上限

```c
struct pud_caps {
    u32 magic;          /* PUD_CAPS_MAGIC = 0x43445550 ("PUDC") */
    u32 proto_ver;      /* PUD_PROTO_VER = 2（固件与驱动必须一致） */
    u32 frame_max;      /* 单次 EP1 传输上限（含 12 B header）：RP2350 65536 / RP2040 32768 */
    u32 decoder_type;   /* PUD_DECODER_*: 0 tjpgd, 1 JPEGDEC, 2 LZ4, 3 QOI, 4 RLE */

    /* 前 16 字节之后的字段是后来追加的；只回 16 字节的老固件仍然合法。 */
    u16 xres;           /* 面板在它被驱动的坐标系下的尺寸 */
    u16 yres;
    u16 pixelclock_khz;
    u8  rotation;       /* 固件应用的 TFT_ROTATION */
    u8  bpp;
    u8  intf_type;
    u8  tp_polling_period; /* 触摸轮询周期，ms；没有触摸驱动时为 0 */
    u16 width_mm;       /* 面板有效区，输入设备用它算分辨率（0=未知） */
    u16 height_mm;
    u16 flags;          /* PUD_CAPS_TOUCH：设备真的有触摸控制器 */
};
```

用途：**分带大小由设备决定**，不再写死在驱动里。固件侧同一个数字决定
`EP1_RD_BUF_SIZE` 和帧槽大小（`PUD_MAX_TRANSFER`），而它按板子不同 —— RP2040 只有
256 KB SRAM，塞不下 RP2350 的 128 KB + 2×64 KB。这样一份驱动就能同时服务两种板子。

- `pud_read_caps()` 把结果落到 `pud->frame_max` / `pud->max_band_pixels`
  （= `(min(USB_TRANS_MAX_SIZE, frame_max) - 12 - 16) / 3`，QOI 最坏 3 B/px + 16 B 头尾
  + 12 B EP1 header），`pud_fb_dirty()` 用它算 `rows`。
- `decoder_type` 决定**主机用哪个编码器**（`pud_encode_band()`）：3 → QOI，4 → RLE；
  0/1/2 目前会报错并置 `needs_full_refresh`（见
  [display-and-refresh.md](display-and-refresh.md)）。设备没报能力时按 QOI（固件默认）。
  这个数字是协议字段，**不要重排**。
- **面板参数也来自这里**（`pud_caps_to_display()`）：分辨率、旋转、bpp、总线时钟都写进
  每台设备自己的 `pud->display_data`（`pud->display` 指向它），DRM mode 与输入设备的
  轴范围都由它推出来 —— 驱动里**不再有写死的 480×320**。查询发生在后端分配之前
  （`pud_query_caps()`，用一个临时堆对象当 DMA 缓冲），所以 `pud_drm_alloc()` /
  `pud_framebuffer_alloc()` 拿得到参数。
- 老固件（只回 16 字节）→ 面板参数保持 `pud_default_display`（480×320/旋转 0/16 bpp），
  日志里写 `old firmware: no panel parameters`。
- 响应缓冲直接用 `pud->ctrl_buf`（16 B，嵌在堆上的 `struct pud` 里 → DMA 可映射 ✓，
  符合"传输 buffer 不能是栈"的约束），并 `BUILD_BUG_ON` 保证结构体不超过它。
- **必须校验 `magic`**：老固件没有这条命令，会用 EP2 缓冲里的残留内容应答
  （实测新固件上 `cmd=0x7f` 曾把上一次的 caps 原样发回），所以"收到 16 字节"不等于
  "设备支持该命令"。magic 不匹配就保留 `PUD_DEFAULT_BAND_PIXELS` 并 `dev_warn`。
- RP2350 上 `frame_max=65536` 经 `min(65535, …)` 得到 **21835** px/band
  （`(65535 - 12 - 16) / 3`，与 `PUD_DEFAULT_BAND_PIXELS` 同一个式子；真机实测
  `caps: proto 2, frame_max 65536, decoder 3 -> 21835 pixels per band`）。
  v1 日志里这个数是 **21839**（`(65535 - 16) / 3`）—— 那时 12 B EP1 header 还不占
  单次传输的预算，引用旧日志时别把它当成现在的分带上限。

## 修改协议时的检查清单

改动任何字段都要同步这四处：

1. 驱动 `usb.c` 的结构体/填充函数（如 `pud_feed_ctrl_buf()`）
2. 固件 `src/cherryusb/usbd_vendor.c` 的对应结构体与 `vendor_request_handler()`
3. 本文件与 `Pico-USB-Display/notes/usb-protocol.md`
4. 两侧的缓冲尺寸常量（`USB_TRANS_MAX_SIZE` ↔ `DECODER_FRAME_MAX`）

**字节序**：所有多字节字段一律小端。驱动跑在 aarch64（小端）上，固件是 Cortex-M33（小端），
目前没有字节序转换代码 —— 如果将来主机跑在大端平台，这里全是坑。
