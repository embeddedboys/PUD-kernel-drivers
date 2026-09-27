# 踩坑合集

按主题分组。每条都是**实际发生过**的问题，附带现象、根因、修法。

---

## 一、DMA 与内存

### 1.1 `transfer buffer is on stack`（内核 WARN）

**现象**：`insmod` 时 dmesg 出现

```
WARNING: CPU: ... at drivers/usb/core/hcd.c:1503 usb_hcd_map_urb_for_dma+0x...
transfer buffer is on stack
```

**出处**（`drivers/usb/core/hcd.c:1502`）：

```c
} else if (object_is_on_stack(urb->transfer_buffer)) {
        WARN_ONCE(1, "transfer buffer is on stack\n");
        ret = -EAGAIN;
}
```

要看懂它，得知道进入这一行的条件链（同一个 `if/else if` 链，必须全部满足）：

| # | 条件 | 说明 |
| --- | --- | --- |
| 1 | `urb->transfer_buffer_length != 0` | 有数据阶段；纯控制请求传 `NULL, 0` 不会进来 |
| 2 | 未设 `URB_NO_TRANSFER_DMA_MAP` | 没有自己提供 DMA 地址 |
| 3 | `hcd->localmem_pool == NULL` | xhci 不设 localmem_pool |
| 4 | `hcd_uses_dma(hcd)` | `CONFIG_HAS_DMA && (hcd->driver->flags & HCD_DMA)` |
| 5 | `num_sgs == 0 && sg == NULL` | 线性 buffer，而非 SG 表 |
| 6 | `object_is_on_stack(transfer_buffer)` | ← **就是这条** |

第 4 条确认：`drivers/usb/host/xhci.c:5504` 里 `.flags = HCD_MEMORY | HCD_DMA | HCD_USB3 | ...`，
板子上的 Pico 挂在 `fc400000.usb`（xhci-hcd），所以这个检查**生效**。

另外注意执行顺序 —— `usb_hcd_submit_urb()` 中映射在入队**之前**：

```
status = map_urb_for_dma(hcd, urb, mem_flags);      /* 这里 WARN */
...
status = hcd->driver->urb_enqueue(hcd, urb, mem_flags);
```

所以控制器**根本没看到这个 URB**，传输完全没发生，`usb_submit_urb()` 返回 `-EAGAIN`。

**本项目的历史现场**：`pud_probe()` 里

```c
u8 serial[8];                                      /* ← 栈上 */
pud_read_unique_id(intf, serial, ARRAY_SIZE(serial));
pr_info("sn : 0x%02x...", serial[0], ...);
```

`pud_read_unique_id()` → `pud_transfer()` → `usb_bulk_msg(..., (void *)data, ...)`，
`data` 就是那个栈数组。

**当时的可见症状**：`usb_start_wait_urb()` 拿到 `-EAGAIN` 后 `goto out`，
把 `*actual_length` 置 0 返回；`serial[]` 从未被设备写过，
于是 `pr_info` 打印出**内核栈残留字节**（每次启动都不同，且是一次内核栈信息泄漏）。
因为是 `WARN_ONCE`，每次启动只打印一次 —— 重启后不再出现 ≠ 问题消失。

**修法**：改用堆内存。

```c
u8 *serial = kzalloc(8, GFP_KERNEL);
if (!serial) return -ENOMEM;
...
kfree(serial);
```

**为什么必须拒绝（不是内核洁癖）**：
- `dma_map_single()` 假设指针在直接映射区（`virt_addr_valid`）；
- 本板 `CONFIG_VMAP_STACK=y`，内核栈在 vmalloc 空间，所以**同时**会命中 1.2 的检查；
  USB 核心先查 `object_is_on_stack()`，给了更明确的提示；
- 栈是普通可缓存映射，DMA 一致性假设被打破；
- 异步 URB 提交后调用者立即返回，栈帧随时可能被复用；
- `object_is_on_stack()` 只对比 **`current`** 的栈（`include/linux/sched/task_stack.h:90`），
  **跨上下文提交的栈 buffer 检测不到** —— 所以内核选择直接失败而不是赌。

### 1.2 `rejecting DMA map of vmalloc memory`

**出处**（`include/linux/dma-mapping.h:332`）：

```c
if (dev_WARN_ONCE(dev, is_vmalloc_addr(ptr),
                  "rejecting DMA map of vmalloc memory\n"))
        return DMA_MAPPING_ERROR;
```

**现场**：`pud->tx_buf` / `pud->encoder_buf` 最初用 `kmalloc` 分配（显示分辨率较大时
直接 "page allocation failure: order:7"），改成 `vmalloc` 之后又撞上这条 ——
`encoder_buf` 要作为 EP1 bulk 传输的 DMA 源（同步 `usb_sg` 与异步 URB 都一样）。

**修法**：区分用途。
- `tx_buf` 只给 CPU 读写 → `vmalloc` 没问题；
- `encoder_buf` 要被 DMA 读 → 必须 `dma_alloc_coherent`。之后同步路径用
  `vmalloc_to_page()` 取底层页组 SG 表，异步路径把 `encoder_dma` 直接填进 URB。

**通用规则**：交给 `usb_submit_urb` / `usb_control_msg` / `usb_bulk_msg` /
`usb_interrupt_msg` / `usb_fill_*_urb` / `usb_sg_init` 的 buffer 必须是：

- ✅ `kmalloc`/`kzalloc`/`kvmalloc`/`__get_free_pages`/`dma_alloc_coherent`，或由这类内存组成的 SGL
- ❌ 栈（`object_is_on_stack`）
- ❌ vmalloc/vmap（`is_vmalloc_addr`）
- ⚠️ 或者设 `URB_NO_TRANSFER_DMA_MAP` 并自己填 `transfer_dma`

### 1.3 本驱动所有 USB 传输 buffer 的来源（自查表）

改动涉及 USB 传输时应回归这张表：

| 位置 | 通道 | buffer | 结论 |
| --- | --- | --- | --- |
| `pud_transfer()` EP0 | control OUT | `pud->ctrl_buf`（嵌在 `struct pud`，`devm_drm_dev_alloc` → 堆） | ✅ |
| `pud_transfer()` EP2 | bulk IN | 调用者传入（`pud_read_unique_id` → `kzalloc(8)`；`pud_query_caps` → 调用者的 `kzalloc`） | ✅ |
| `pud_set_params()` EP0 | control OUT | `pud->ctrl_buf`（命令头 + `struct pud_params`） | ✅ |
| `pud_set_params()` EP2 | bulk IN | **`pud->ctrl_buf`**（回读 `struct pud_param_state`，再 memcpy 给调用者） | ✅ |
| `pud_flush()` EP1（同步） | bulk OUT（`usb_sg_init`） | `pud->bulk_sgt.sgl` → `dma_alloc_coherent` 的页 | ✅ |
| `pud_flush()` EP1（异步） | bulk OUT（`usb_submit_urb`） | `pud->encoder_buf`，`transfer_dma = encoder_dma` + `URB_NO_TRANSFER_DMA_MAP` | ✅ |
| `input.c` 触摸 | int IN（`usb_fill_int_urb`） | `pud->ep_int_buf` = `kzalloc` | ✅ |

> 这张表里曾有一行 `REQ_EP4_IN`(0x05) 控制传输：那个轮询式取触摸的做法已经删掉
> （现在是设备主动推送，见 [usb-protocol.md](usb-protocol.md) 的 EP4 一节），
> 代码里只剩 `pud.h` 的宏定义。
>
> `pud_set_params()` 那两行是后加的，**加的时候正好踩了这条**：第一版把回读的目标
> 写成调用者栈上的 `struct pud_param_state`，实机 `insmod` 立刻
> `WARN ... transfer buffer is on stack`（`usb_hcd_submit_urb`），那笔读取也被拒
> （`-EAGAIN`），参数于是没生效。新加"查询类"命令时，**回读缓冲必须用 `ctrl_buf`
> 或 `kzalloc`**，栈上的只允许当 memcpy 的目标。

**容易误判的一处**：同步路径（`PUD_USB_ASYNC=0`）的 `pud_flush()` 里

```c
struct pud_usb_bulk_context ctx;   /* 含 usb_sg_request + timer_list，确实在栈上 */
```

看着像同类错误，但其**是描述符不是传输 buffer**：`usb_sg_init()` 内部自己
`usb_alloc_urb()`，URB 的 buffer 指向 SGL 里的 coherent 内存；
`timer_setup_on_stack()`/`destroy_timer_on_stack()` 也是短生命周期 timer 的官方用法。
**不要"顺手"改成堆分配**（栈上 timer 是刻意避免 kmalloc 失败路径）。

异步路径的 `struct pud_ep1_async_ctx`（completion + status + actual）同样在栈上，同样合法：
调用方会一直等到 completion 或 `usb_kill_urb()` 返回才离开函数，URB 在途时栈帧不会被复用。
**这是本函数自己保证的前提**——异步接口本身不保证，谁要改成"提交后不等待就返回"，
就必须把 context 挪到能活过传输的地方。

### 1.4 swiotlb "buffer is full"

**现象**：大量 `swiotlb buffer is full (sz: 4)`，伴随 gnome-shell 卡顿。

**根因**：接口的流式 DMA mask 只有 32 位，超出范围的 buffer 要走 swiotlb bounce buffer，
高刷新率下把 bounce 池打爆。

**修法**：把**流式** DMA mask 提到 64 位，coherent 保持 32 位：

```c
/* streaming: 64-bit, coherent: 32-bit */
```

### 1.5 `order:7` 页分配失败

`kmalloc` 一次要 128KB 连续物理页（order 7）在系统跑起来后基本会失败。
大块缓冲一律 `vmalloc`（CPU 用）或 `dma_alloc_coherent`（DMA 用），不要 `kmalloc`。

---

## 二、USB 传输细节

### 2.1 `pud_transfer()` 丢掉 EP0 的返回值（**未修**）

```c
int rc, actual_length;

rc = usb_control_msg(...);          /* ← 失败也不管：rc 紧接着被覆盖 */
rc = usb_bulk_msg(udev, pipe, (void *)data, len, &actual_length,
                  PUD_DEFAULT_TIMEOUT);
if (rc)
        return rc;
return actual_length;
```

- **返回值本身现在是安全的**。曾经担心的是"`actual_length` 未初始化就被返回"：
  `usb_bulk_msg()`（`drivers/usb/core/message.c`）确实有两条提前返回、不写
  `*actual_length` 的路径（`!ep || len < 0` → `-EINVAL`，`usb_alloc_urb()` 失败 →
  `-ENOMEM`），但它们都返回**非零** rc，而现在的代码遇到非零 rc 就直接返回 rc，
  走不到 `return actual_length`。
- **仍然成立的一条**：EP0 控制请求（那 4 字节请求头）失败时被无视，照样去发 bulk ——
  设备可能拿上一次的请求来解释这一笔。改法就是 `if (rc < 0) return rc;`。
- `pud_read_unique_id()` 的返回值在 `pud_probe()` 里**没人查**：读失败只会打印 8 个
  `0x00`，毫无提示（序列号只进 dmesg，所以影响有限）。

### 2.2 `pud_flush()` 里未检查的 EP0 控制请求（**已随 v2 消失**）

v1 是"EP0 窗口协商 + EP1 数据"两次传输，控制请求失败却继续发 payload，设备就会用
**上一个矩形窗口**画新数据。v2 把 12 字节头放进 `encoder_buf`、与载荷**一次 bulk 传完**，
这条路连同这个坑一起没有了 —— 现在 `pud_flush()` 里根本没有控制请求。

### 2.3 "16 字节 wLength 配 12 字节结构体"（**已消失**）

那条说的是 v1 的窗口请求：`usb_control_msg(..., sizeof(pud->ctrl_buf) /* 16 */, ...)`
配一个只有 12 字节的 `struct req_ep1_out`。**`req_ep1_out` 现在全仓不存在**（`grep` 只剩
这句话自己）。其中仍然成立的是**取偶**那一半：`pud_flush()` 会把 `size` 向上取到偶数
（`if (data_size % 2) data_size += 1;`）—— 设备奇偶都收，取偶只为兼容 2026-09 之前会拒绝
奇数 `size` 的老固件，见 [usb-protocol.md](usb-protocol.md) 的"`size` 的奇偶"。

---

## 三、DRM / 显示接口

### 3.1 `IS_ERR()` 而不是 `!ptr`

`pud_drm_alloc()` 失败返回 `ERR_PTR(-ENOMEM)`，调用方写成 `if (!drm)` 会把错误指针
当有效设备用 → `insmod` 时 SIGSEGV / oops（现场：`pud_probe+0x80`）。必须 `IS_ERR()`。

### 3.2 6.1 与 6.12 的 API 差异

| 6.12 | 6.1 |
| --- | --- |
| `drm/drm_fbdev_dma.h` | `drm/drm_fb_helper.h` |
| `drm_fbdev_dma_setup()` | `drm_fbdev_generic_setup()` |
| `drm_fb_xrgb8888_to_rgb565(..., fmtcnv_state)` | 少一个参数 |
| `drm_shadow_plane_state.fmtcnv_state` | 不存在 |
| 自定义 pipe 回调 | `DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS` |

### 3.3 `Format is not supported: RG16 little-endian (0x36314752)`

`drm_fb_memcpy()` 的默认分支不认 `DRM_FORMAT_RGB565`。合成器（以及 16bpp 的 fbdev 模拟）
会把 RGB565 的 fb 交上来，所以 `pud_buf_copy()` **必须**显式处理这个 case：

```c
case DRM_FORMAT_RGB565:
    drm_fb_memcpy(&dst_map, NULL, src, fb, clip);
    break;
```

### 3.4 gnome-shell 不点亮面板 / monitors.xml

合成器把连接器名字记在 `~/.config/monitors.xml`。Pico 换 USB 端口或重新枚举后，
连接器名可能从 `USB-1` 变成别的（曾出现记录的 `Unknown20-1` 与实际 `USB-1` 不符），
于是 gnome-shell 认为"这个显示器不在配置里"而不点亮。

**修法**：删掉 `monitors.xml` 并重启会话，让它重新探测。

### 3.5 局部刷新有残影 ≠ 驱动问题

一次真实误判：局部刷新残影的根因在**固件丢帧**（帧槽满时静默丢帧），
驱动侧无需改动。排查顺序见 [display-and-refresh.md](display-and-refresh.md)
最后一节。

### 3.6 7.0：没有 `.fbdev_probe` 就没有 `/dev/fb0`

7.0 把 fbdev 模拟拆成两半：`drm_client_setup()` 只**注册客户端**，真正分配 fbdev 后备存储
（以及设置 `fb_helper->funcs`）的是驱动的 `.fbdev_probe`，由 `DRM_FBDEV_DMA_DRIVER_OPS` 提供。
`drm_fb_helper_single_fb_probe()` 第一句就是：

```c
if (drm_WARN_ON(dev, !dev->driver->fbdev_probe))
	return -EINVAL;
```

**症状**：`insmod` 成功、DRM 设备注册成功、`/dev/dri/card0` 在，但**没有 `/dev/fb0`**、
fbcon 不接管，dmesg 里只有一条 `WARNING`。移植时代码能编过、probe 也不报错，所以很容易漏。
6.1/6.12 的 `drm_fbdev_generic_setup()` / `drm_fbdev_dma_setup()` 是自己把这件事做掉的，
7.0 换成了这一对组合。

---

## 四、模块生命周期

### 4.1 `disagrees about version of symbol module_layout`

模块 `vermagic` 与运行内核不一致。必须用**运行内核**的 headers/objtree 编译。
`modinfo pud.ko | grep vermagic` 与 `uname -r` 对照。

### 4.2 `rmmod` 失败、refcnt 只增不减

`refcnt` 会被 gnome-shell 持有的 `/dev/dri/cardN` fd 抬高（曾涨到 32）。
**可靠的重置手段是重启开发板**；停显示管理器、解绑 vtconsole 不一定管用。

### 4.3 修改内核源码树的诱惑

构建 6.1.172 时需要用 `KERN_OBJ_DIR` 指向 objtree，而不是去 `KERN_DIR` 里补文件。
**`KERN_DIR` 只读**。

### 4.4 固件多一个接口，驱动就多注册一个显示设备

`pud_ids[]` 原来是设备级的 `USB_DEVICE(0x2E8A, 0x0001)`。USB 核**按接口**匹配
`struct usb_driver`：一个设备级 id 会让**每个**接口都匹配上，`probe()` 于是被调用多次。
本驱动的端点地址是写死的常量（`EP1_OUT_ADDR` 等，不查描述符），所以第二次 probe 不会失败 ——
固件加上 picoboot 的 reset 接口（`0xff/0x00/0x01`、无端点，为的是让 picotool 能在应用态把板子
送进 BOOTSEL）之后，实测（`make qemu`，同一个固件，只换驱动）：

| | 绑定的接口 | DRM | fb | `Initialized pud-drm` |
| --- | --- | --- | --- | --- |
| 设备级 id（旧） | `1-1:1.0` **+ `1-1:1.1`** | card0 **+ card1** | fb0 **+ fb1** | **2** 次 |
| 接口级 id（现在） | `1-1:1.0` | card0 | fb0 | 1 次 |

第二次 probe 只在注册输入设备时才报错（`device has no interrupt IN endpoint`，因为
`pud_input_setup()` 会查 EP4），DRM/fbdev 两个都注册成功了 —— 也就是**同一块屏被两个 card
驱动**，谁都不知道对方在写。

修法两道：`USB_DEVICE_AND_INTERFACE_INFO(0x2E8A, 0x0001, 0xff, 0x00, 0x00)`（只匹配图像接口，
reset 接口的 protocol 是 1，天然被排除），以及 `pud_probe()` 入口的
`bInterfaceProtocol != 0 → -ENODEV`（万一以后固件再加接口）。
**加接口 / 改接口类码时，这两个地方要一起看。**

### 4.5 7.0：`insmod` 报 `Unknown symbol in module`

7.0 的 fbdev 客户端在 drm 核心里，模块引用的 `drm_fbdev_dma_driver_fbdev_probe` 与
`drm_client_setup` 都出自 `drm.ko`，而 `modinfo` 的 `depends:` 只写着 `drm_dma_helper`：

```
insmod: ERROR: could not insert module pud.ko: Unknown symbol in module
```

两条路：先 `modprobe drm_dma_helper`（它会把 `drm` 带起来）再 `insmod`；或者把模块放进
`/lib/modules/$(uname -r)` 下用 `modprobe pud`，由 `modules.dep` 解决依赖。
`scripts/pud-load.sh` 现在自己会做第一步（读 `modinfo -F depends` 再 `modprobe -a`）；
`modprobe --show-depends <路径>` 对 out-of-tree 的 `.ko` **不**解析依赖，别指望它。
只看 `insmod` 的错误看不出是哪个符号，`dmesg` 里才有：
`pud: Unknown symbol drm_fbdev_dma_driver_fbdev_probe (err -2)`。

### 4.6 `initial_mode=1` 之后卸载卡死（**未定位**）

`insmod pud.ko initial_mode=1` 之后再卸载，会卡在 `pud_drm_unregister` 里：

```
[    4.719804] pud-drm: pud_drm_unregister
<没有下文，`timeout 15 rmmod pud` 之后客户机整个僵住>
```

对比：**同样的路径不带 `initial_mode`（默认）时卸载是干净的**
（`pud_drm_unregister` → `Console: switching to colour *CGA` → `pud_drm_pipe_disable`）。
所以卡的不是 `drm_atomic_helper_shutdown()` 那一段（那会先打 `pipe_disable`），而是它之前的
`drm_dev_unplug()` —— 停顿点连 fbcon 的 CGA 回切都没到。

现场（2026-09）：7.0 的 QEMU 客户机 + 真设备直通，3/3 复现。**6.1 板上只验证过
"面板会被点亮"，没验证过卸载**，所以未必是 7.0 独有的问题。`initial_mode` 默认关闭
（它是给"没有任何用户态来提交模式"的场景用的，见 drm.c 里 `pud_drm_set_initial_mode()`
的注释），常规用法不受影响 —— 但**用完之后别留着它卸载模块/关机**。

## 五、设备/固件状态

### 5.1 面板卡死：caps 与 EP1 全部 `-71`，复位 Pico 即恢复

**症状**（2026-09，CachyOS 本机 + 7.2.7-1-cachyos，插拔/直通之后加载驱动）：

```
pud 1-4:1.0: no capability report (-71), using defaults
pud 1-4:1.0: no capability report (0 bytes); keeping 21835 pixels per band
pud 1-4:1.0: EP1 transfer failed (-71) for 480x45+0+0, 366 bytes
pud 1-4:1.0: EP1 failed 3 times in a row, trying once per second
```

之后每笔 EP1 都是 `-71`，一秒一笔地重试下去。**判据**（用来区分"设备卡住"和"驱动/内核问题"）：

- `-71` 是 `-EPROTO`：**设备把端点 STALL 了**。主机侧的等待超时是 `-110`，别混。
- **连 caps 控制请求都失败**（`no capability report (-71)`），而且失败的第一笔 EP1 只有
  **366 字节** —— 与带宽、分带预算、丢帧无关。
- 于是驱动回退默认值：`keeping 21835 pixels per band` → 日志里的带就是 `480x45`
  （真机 caps 是 `frame_max 32768 -> 10913`，对应 `480x22`）。**看到 `480x45` 就知道 caps 没拿到**。
- 设备本身枚举是好的：`2e8a:0001` 在、`ep_01 Bulk out / ep_82 Bulk in / ep_84 Interrupt in` 都在。

**处理**：**复位 Pico**（或拔插 USB）。复位后驱动会**自动重新 probe**，不用 `rmmod`/`insmod`：

```
usb 1-4: new full-speed USB device number 5 ... Product: Pico USB Display
pud 1-4:1.0: caps: proto 2, frame_max 32768, decoder 3 -> 10913 pixels per band
pud-drm: pud_drm_register
pud 1-4:1.0: [drm] fb1: pud-drmdrmfb frame buffer device
```

之后 EP1 失败归零，面板正常工作（本机是镜像模式）。**成因是推断的**：这次之前刚在 QEMU
客户机里做过真设备直通，客户机断电时留下一笔在飞的 EP1（`EP1 transfer failed (-108)` 那条），
设备很可能停在"等一个永远收不完的传输"的状态上；同一份 `pud.ko`、同一个内核在客户机里
是完整跑通的，所以不像驱动/内核的问题。下次再遇到，先复位设备再查驱动。

### 5.2 EP1 超时能把 xHCI 控制器整个拖死（设备复位救不了，要重启或 unbind/bind）

同一台机器、同一个内核（7.2.7-1-cachyos），本机加载驱动 + 让桌面**镜像**到面板（持续推流）
跑了约 10 分钟之后：

```
00:54:44  xhci_hcd 0000:05:00.3: xHCI host not responding to stop endpoint command
00:54:44  xhci_hcd 0000:05:00.3: xHCI host controller not responding, assume dead
00:54:44  xhci_hcd 0000:05:00.3: HC died; cleaning up
00:54:44  usb 1-3: USB disconnect, device number 2          ← 同一控制器上别的设备也掉
00:54:44  pud 1-4:1.0: EP1 transfer failed (-110) for 480x22+0+44, 2814 bytes
00:54:44  usb 1-4: USB disconnect, device number 5
00:54:44  pud_drm_cleanup → pud_drm_unregister → pud_crtc_atomic_disable
```

要点：

- **死之前只有这一笔失败**：`-110` 是传输超时（不是 §5.1 的 stall `-71`），带是 `480x22`
  —— 也就是 caps 推出的 10913 px 预算、不是兜底值，所以驱动侧算得没错。链条是
  "设备收不动 → 传输超时 → 主机对端点下 stop 命令得不到响应 → 内核判定控制器死亡"。
  这与 AGENTS.md"板子上的工作方式"第 7 条描述的是同一个模式，只是更重：不只是卡住，
  是控制器整个掉了。
- **影响面是一整个控制器**：`0000:05:00.3` 上的设备全掉（面板 `1-4` 和摄像头 `1-3`），
  而且不会自己回来；`pud` 的 disconnect 路径本身是干净跑完的（`pud_drm_unregister` →
  `pud_crtc_atomic_disable`）。旁证：死亡前 1 分钟 dmesg 里有一串
  `pud-drm: pud_crtc_mode_valid`，但**不能**由此断定因果 —— 那只是合成器在频繁探 mode，
  是正常调用。这条打印后来从 `pr_info` 降成 `drm_dbg_kms`（默认不再刷屏；要看就开 DRM
  debug），所以现在的日志里看不到它了。
- **恢复**：重启最干净。不想重启就 unbind/bind 那个 PCI 设备 —— **这次实测有效、没有重启**
  （`uptime` 仍显示 1h33m，两个 xhci 控制器都在、`1-3`/`1-4` 都重新枚举回来）。
  [`scripts/pud-load.sh`](../scripts/pud-load.sh) 把这一套做成了命令：`recover` 自己找死掉的
  控制器并 unbind/bind，`load` 在加载前也会自动做一次（控制器不在的时候面板根本不在总线上，
  先加载没有意义）：

  ```bash
  scripts/pud-load.sh recover                     # 自动找出并救回
  # 手工版本（仓库不在手边时）：
  sudo rmmod pud                                  # 或 scripts/pud-load.sh unload --stop-dm
  echo 0000:05:00.3 | sudo tee /sys/bus/pci/drivers/xhci_hcd/unbind
  echo 0000:05:00.3 | sudo tee /sys/bus/pci/drivers/xhci_hcd/bind
  ```

  **判据**：控制器被清理之后，root hub（`usbN`）不再挂在 PCI 设备下面，而 PCI 设备仍然绑着
  `xhci_hcd` —— 所以"绑着 xhci_hcd 却没有 `usbN` 子节点"就是死的。脚本里这一句必须用
  `compgen -G "$d/usb*"` 这类写法，**不能**写 `[ -e "$d"/usb* ]`：USB3 控制器有两个 root hub
  （`usb1 usb2`），`[ -e ]` 拿到两个展开路径会报错、按失败算 —— 于是**健康的控制器全被误判成
  死的**，每次 `load` 都会去重绑一遍（写这个脚本时真踩了，已修）。

  绑回来还是"不响应"就别再反复试探了（那是硬件/固件级僵死），只能重启。
- **结论**：显示类测试继续走客户机（`make qemu`，见 [build-and-test.md](build-and-test.md)
  C/D 节），本机最多 `scripts/pud-load.sh load input_only=1`（没有 DRM 节点就没有 EP1 流量）。
  **"桌面镜像到面板"这种持续高负载推流是最容易触发它的用法** —— §5.1 的复位只能救回设备，
  救不回控制器。
