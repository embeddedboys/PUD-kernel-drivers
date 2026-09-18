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
| EP0 | control | 厂商请求（窗口协商、查询命令） | 使用中 |
| EP1 | OUT / bulk | **图像压缩流**（QOI 帧数据） | 使用中 |
| EP2 | IN / bulk | 查询响应（如读序列号） | 使用中 |
| EP3 | OUT / bulk | — | 固件已定义 `REQ_EP3_OUT`/`EP3_OUT_ADDR`，但**未写进配置描述符**，未实现 |
| EP4 | IN / interrupt (64B, bInterval 33) | 触摸数据 | 固件侧打桩，未真正上报 |

## 请求号

固件 `usbd_vendor.h` 与驱动 `pud.h` 中的定义完全一致：

```
REQ_EP0_OUT = 0x00      REQ_EP0_IN  = 0x01
REQ_EP1_OUT = 0x02      REQ_EP2_IN  = 0x03
REQ_EP3_OUT = 0x04      REQ_EP4_IN  = 0x05
```

驱动的 `bmRequestType` 固定为 `TYPE_VENDOR | USB_DIR_OUT` = `0x40`（厂商请求、主机→设备、设备接收者）。

> 驱动里 `REQ_EP0_OUT` / `REQ_EP0_IN` 只是照抄固件定义，**没有任何调用点**（死代码）。

## 数据通道 1：EP1 图像帧

### 时序

主机每次都**先发一个 EP0 控制请求做"窗口协商"，再从 EP1 批量发出数据**：

```
usb_control_msg(REQ_EP1_OUT, bmRequestType=0x40, payload=struct req_ep1_out)
usb_sg_init/usb_sg_wait → EP1 bulk OUT  (压缩流本体)
```

### 控制请求载荷

驱动 `usb.c` 的 `struct req_ep1_out`（小端）：

```c
struct req_ep1_out {
    u16 xs;     /* 起始列（含） */
    u16 ys;     /* 起始行（含） */
    u16 xe;     /* 结束列（含！不是排他） */
    u16 ye;     /* 结束行（含！） */
    u32 size;   /* 随后 EP1 批量数据的字节数 */
};
```

- `sizeof(struct req_ep1_out)` = **12** 字节（无填充）。
- `xe`/`ye` 是**闭区间**：驱动传来的是 `band.x2 - 1` / `band.y2 - 1`。
  固件据此算出 `width = xe - xs + 1`。
- 坐标是**相对于整屏**的绝对坐标，固件内部通过 `ctx.ox/oy` 偏移。

### ⚠️ 已知不一致：控制请求的 wLength 是 16 而不是 12

驱动发送时用的是整个控制缓冲区长度：

```c
usb_control_msg(..., REQ_EP1_OUT, TYPE_VENDOR | USB_DIR_OUT, 0, 0,
                pud->ctrl_buf, sizeof(pud->ctrl_buf) /* = 16 */, timeout);
```

而 `pud->ctrl_buf` 是 `u8 ctrl_buf[16]`，`pud_feed_ctrl_buf()` 只填前 12 字节。
所以 **wLength = 16，其中只有前 12 字节有意义**，后 4 字节是残留数据。
固件只按 `struct req_ep1_out` 读 12 字节，因此现在能正常工作 —— 但这是个隐式约定，
改协议时容易踩。要清理的话，应把 `sizeof(pud->ctrl_buf)` 换成 `sizeof(struct req_ep1_out)`。

### ⚠️ 已知不一致：size 会被向上取偶

`pud_flush()` 开头：

```c
/* data_size must be even for RP2350 */
if (data_size % 2)
    data_size += 1;
```

于是 `req_ep1_out.size` 可能比真实 QOI 长度大 1，固件会多收到一个尾随垃圾字节。
QOI 解码在读到结束标记（`00..01`）后即停止，多余字节不会被消费，所以无害。
新协议不要依赖"size 精确等于编码长度"。

### 传输上限

| 常量 | 值 | 位置 | 含义 |
| --- | --- | --- | --- |
| `USB_TRANS_MAX_SIZE` | 65535 | 驱动 `pud.h` | 主机侧单次 `size` 上限（受 `u16`/协议约束） |
| `PUD_DEFAULT_BAND_PIXELS` | `(65535-16)/3` = 21839 | 驱动 `pud.h` | 拿不到能力报告时的兜底单带像素数 |
| `pud->max_band_pixels` | 21839（RP2350） | 驱动 `drm.c` | **实际使用的**单带上限，来自 `PUD_CMD_GET_CAPS` |
| `PUD_MAX_TRANSFER` | 65536 / 32768 | 固件 `usbd_vendor.h` | **按板子**定的单次传输与帧槽上限（RP2350 / RP2040） |
| `DECODER_FRAME_SLOTS` | 2 | 固件 `decoder.c` | 帧槽数量（双缓冲，尺寸 = `PUD_MAX_TRANSFER`） |

**关键约束**：一帧的压缩结果必须 ≤ **设备上报的** `frame_max`。固件在 REQ_EP1_OUT
控制阶段就会校验（超限直接 stall EP0，见 `usbd_vendor_ep1_size_ok()`），不再静默截断。
驱动靠分带（见 [display-and-refresh.md](display-and-refresh.md)）保证这一点。

### 流控（很重要）

固件**不会**无条件接收 EP1 数据。当解码帧槽全忙时，它收到 EP1 控制请求后
**故意不武装 EP1 读取**，主机的批量传输因此阻塞等待，直到解码任务腾出槽位才续上。

对驱动而言这是透明的：`usb_sg_wait()` 只是多等一会儿。但要注意：

- `pud_flush()` 里有个 **3 秒看门狗**（`pud_usb_bulk_timeout()` → `usb_sg_cancel()`）。
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
    u32 proto_ver;      /* PUD_PROTO_VER = 1 */
    u32 frame_max;      /* 单次 EP1 传输上限：RP2350 65536，RP2040 32768 */
    u32 decoder_type;   /* 0 unused, 1 JPEGDEC, 2 LZ4, 3 QOI */
};
```

用途：**分带大小由设备决定**，不再写死在驱动里。固件侧同一个数字决定
`EP1_RD_BUF_SIZE` 和帧槽大小（`PUD_MAX_TRANSFER`），而它按板子不同 —— RP2040 只有
256 KB SRAM，塞不下 RP2350 的 128 KB + 2×64 KB。这样一份驱动就能同时服务两种板子。

- `pud_read_caps()` 把结果落到 `pud->frame_max` / `pud->max_band_pixels`
  （= `(min(USB_TRANS_MAX_SIZE, frame_max) - 16) / 3`，QOI 最坏 3 B/px + 16 B 头尾），
  `pud_fb_dirty()` 用它算 `rows`。
- 响应缓冲直接用 `pud->ctrl_buf`（16 B，嵌在堆上的 `struct pud` 里 → DMA 可映射 ✓，
  符合"传输 buffer 不能是栈"的约束），并 `BUILD_BUG_ON` 保证结构体不超过它。
- **必须校验 `magic`**：老固件没有这条命令，会用 EP2 缓冲里的残留内容应答
  （实测新固件上 `cmd=0x7f` 曾把上一次的 caps 原样发回），所以"收到 16 字节"不等于
  "设备支持该命令"。magic 不匹配就保留 `PUD_DEFAULT_BAND_PIXELS` 并 `dev_warn`。
- RP2350 上 `frame_max=65536` 经 `min(65535, …)` 得到 21839 px/band，与改前的编译期
  常量**逐字节相同** → 行为零变化（真机实测 `caps: proto 1, frame_max 65536,
  decoder 3 -> 21839 pixels per band`）。

## 修改协议时的检查清单

改动任何字段都要同步这四处：

1. 驱动 `usb.c` 的结构体/填充函数（如 `pud_feed_ctrl_buf()`）
2. 固件 `src/cherryusb/usbd_vendor.c` 的对应结构体与 `vendor_request_handler()`
3. 本文件与 `Pico-USB-Display/notes/usb-protocol.md`
4. 两侧的缓冲尺寸常量（`USB_TRANS_MAX_SIZE` ↔ `DECODER_FRAME_MAX`）

**字节序**：所有多字节字段一律小端。驱动跑在 aarch64（小端）上，固件是 Cortex-M33（小端），
目前没有字节序转换代码 —— 如果将来主机跑在大端平台，这里全是坑。
