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
| `pud_transfer()` EP2 | bulk IN | 调用者传入（`pud_read_unique_id` → `kzalloc(8)`） | ✅ |
| `pud_flush()` EP1（同步） | bulk OUT（`usb_sg_init`） | `pud->bulk_sgt.sgl` → `dma_alloc_coherent` 的页 | ✅ |
| `pud_flush()` EP1（异步） | bulk OUT（`usb_submit_urb`） | `pud->encoder_buf`，`transfer_dma = encoder_dma` + `URB_NO_TRANSFER_DMA_MAP` | ✅ |
| `input.c` 触摸 | int IN（`usb_fill_int_urb`） | `pud->ep_int_buf` = `kzalloc` | ✅ |

> 这张表里曾有一行 `REQ_EP4_IN`(0x05) 控制传输：那个轮询式取触摸的做法已经删掉
> （现在是设备主动推送，见 [usb-protocol.md](usb-protocol.md) 的 EP4 一节），
> 代码里只剩 `pud.h` 的宏定义。

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
**可靠的重置手段是重启开发板**；`systemctl stop gdm`、解绑 vtconsole 不一定管用。

### 4.3 修改内核源码树的诱惑

构建 6.1.172 时需要用 `KERN_OBJ_DIR` 指向 objtree，而不是去 `KERN_DIR` 里补文件。
**`KERN_DIR` 只读**。

### 4.4 7.0：`insmod` 报 `Unknown symbol in module`

7.0 的 fbdev 客户端在 drm 核心里，模块引用的 `drm_fbdev_dma_driver_fbdev_probe` 与
`drm_client_setup` 都出自 `drm.ko`，而 `modinfo` 的 `depends:` 只写着 `drm_dma_helper`：

```
insmod: ERROR: could not insert module pud.ko: Unknown symbol in module
```

两条路：先 `modprobe drm_dma_helper`（它会把 `drm` 带起来）再 `insmod`；或者把模块放进
`/lib/modules/$(uname -r)` 下用 `modprobe pud`，由 `modules.dep` 解决依赖。
只看 `insmod` 的错误看不出是哪个符号，`dmesg` 里才有：
`pud: Unknown symbol drm_fbdev_dma_driver_fbdev_probe (err -2)`。
