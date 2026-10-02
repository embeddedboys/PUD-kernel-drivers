# EP4 触摸输入

> 触摸是设备主动推送（一条常驻 interrupt IN URB）；设备侧可选，`PUD_CAPS_TOUCH` 没置位就不注册
> 输入设备。`report_mode` 决定注册成触摸屏还是绝对指针。

## TL;DR

- 一条常驻 URB 常挂，回调里**除 `-ENOENT`/`-ECONNRESET`/`-ESHUTDOWN` 外都要重新提交**（用 `GFP_ATOMIC`）。
- `PUD_CAPS_TOUCH`（flags bit0）没置位**不要注册输入设备** —— 否则给用户一个"存在但永远不动"的设备。
- 绝对设备**必须给分辨率**（`input_abs_set_res()`），否则 libinput 判定为驱动 bug。
- `touch` 用 MT-B + `INPUT_PROP_DIRECT`；`pointer` 用 `BTN_LEFT/RIGHT` + `ABS_X/Y`，不置 `DIRECT`。
- 清理顺序：`usb_kill_urb()`（等回调）→ `usb_free_urb()` → `kfree(ep_int_buf)`；
  `pud->indev` 来自 `devm_input_allocate_device()`，**不要**再手动 `input_unregister_device()`（重复注销）。

## 能力位与注册

只有能力位为 1 时 `pud_input_setup()` 才跑；否则日志
`device reports no touch controller, not registering input`。
`pico-display-lib` 里多数板级配置是 `INDEV_DRV_NOT_USED=1`，那种固件既不置位、
`tp_polling_period` 也报 0，EP4 永远不发帧。

协议帧格式（8 字节）见 [usb-protocol.md](usb-protocol.md) 的 "EP4 触摸上报"。
**不要再为每个样本发 `REQ_EP4_IN` 控制请求** —— 那是旧协议做法，会把设备推送节奏拖成每个样本一次
控制传输（驱动里现在只剩 `pud.h` 的宏定义，无调用点）。

## `report_mode`：两种都是绝对设备

| 模式 | 事件集 | libinput 判定 | 桌面表现 |
| --- | --- | --- | --- |
| `touch`（默认） | `BTN_TOUCH` + `ABS_X/Y` + **MT-B**（`ABS_MT_SLOT/POSITION_X/POSITION_Y/TRACKING_ID`）+ `INPUT_PROP_DIRECT` + 分辨率 | `Capabilities: touch` | 摸面板 = 点面板上那个位置；但**绑到哪个输出由合成器决定**，GNOME 对外接触摸屏无可配映射（mutter#2080/#3480），绑错屏时只能改用 `pointer` 或把它变成主输出 |
| `pointer` | `BTN_LEFT/BTN_RIGHT` + `ABS_X/Y` + 分辨率，**不置** `DIRECT`、不报 `BTN_TOUCH`、不建 MT | `Capabilities: pointer` | 绝对指针（QEMU `usb-tablet` 那类），坐标映射到**整个虚拟桌面**：一定能用，代价是"摸面板"≠"点面板" |

`pointer` 的已知副作用：没有 `INPUT_PROP_DIRECT` 时内核 `joydev` 也会绑上这个设备
（`/proc/bus/input/devices` 多一个 `js0`）。不影响当指针用，只是多一个摇杆节点。

## libinput 绝对轴的两条硬要求

都来自 [libinput 绝对轴文档](https://wayland.freedesktop.org/libinput/doc/latest/absolute-axes.html)：

1. **绝对设备必须给分辨率**，否则 libinput 直接认为是驱动 bug。驱动用设备上报的
   `width_mm/height_mm` 算 units/mm 并 `input_abs_set_res()`。`absinfo.resolution` 是整数，所以
   libinput 反推的尺寸会差几个百分点（74×49 mm 的面板被报成 80×46 mm）。拿不到尺寸就不设并告警。
2. 触摸屏用 `INPUT_PROP_DIRECT` 判定；单点屏 libinput 也支持，但 MT-B 才是标准形态
   （手势/长按/拖拽的 tracking 靠 `ABS_MT_TRACKING_ID`，松手必须发 `-1`）。

## 多显示器下触摸屏绑到哪块屏

绝对输入设备必须由合成器绑到某块输出，**没绑上时触摸坐标会被铺满整个屏幕**（所有显示器包围盒），
症状是"摸副屏、点到大屏上、副屏窗口失焦"。Mutter（`meta-input-mapper.c`）只认三件事：设备名里的
EDID 厂商/型号串、**设备尺寸与输出尺寸相差 <5%**、"内建屏"；都没有就保持未绑定，而 GNOME 46
也没有把设备绑到输出的用户界面（`InputMapping` 只有读接口）。

驱动因此让**尺寸**这条成立（它不看设备名，比名字匹配稳）：给 connector 声明物理尺寸
（`display_info.width_mm/height_mm`，取输入设备由整数分辨率反推的尺寸，两边精确一致），并附一份最小
**EDID**（厂商 `PUD`、名称 "pud touch pan"、7×4cm、无 detailed timing，不影响我们自己的 mode）。
没有 EDID 时 Mutter 看到的厂商/型号/序列号全是 NULL，`match_size` 也没有数据 —— 两个都改才生效。

EDID 只以整厘米记尺寸，内核分辨率是整数量/mm，两个粒度要对上就得凑数字：固件因此上报 **70×40mm**
（玻璃实际约 74×49），分辨率取 7/8 → 反推 68.6×40mm，与 EDID 的 70×40mm 差 2.1%/0% ✓。
只影响上报的设备尺寸与 DPI 估算，不影响坐标。

> 备用手段（自动匹配不成立时）：显式写 `output` 键 —— Mutter 里 `META_MATCH_CONFIG` 优先级最高且
> **不依赖尺寸**：
> ```
> gsettings set org.gnome.desktop.peripherals.touchscreen:/org/gnome/desktop/peripherals/touchscreens/<vendor>:<product>/ \
>   output "['PUD', 'pud touch pan', '0x00000001']"
> ```
> `<vendor>:<product>` 是设备 id（`/sys/class/input/eventN/device/id/{vendor,product}`）。
> 同样**没有 EDID 就没法用**：那时厂商/型号/序列号是 NULL，比不出相等。

## 实测（2026-09，真机 + 板子上的 libinput 1.25.0）

`touch` 模式 libinput 报 `Capabilities: touch`、`Size: 80x46mm`，8 次拖动共 1000+ 个 MT 点、
tracking id 递增、每次松手 `-1`、`BTN_TOUCH` 各 8 次按下/松开、采样间隔中位 **8.0 ms**；
`pointer` 模式 libinput 报 `Capabilities: pointer`，一次 5.0 秒拖动 432 个坐标点
（x 26..372、y 76..270）、只有 **2 个 `BTN_LEFT`**（按下 + 松开）、没有 `BTN_TOUCH`/`ABS_MT_*`，
间隔同样中位 **8.0 ms**。

## 相关

- [usb-protocol.md](usb-protocol.md)：EP4 帧格式、`bInterval` 与轮询周期、采样率实测。
- [architecture.md](architecture.md)：`input_only` / `report_mode` 运行期参数。
- [board-testing.md](board-testing.md)：只调触摸时的加载与事件读取命令。
- 固件仓 `Pico-USB-Display/notes/`：触摸控制器轮询实现。
