# 用 usbmon 看驱动到底往设备发了什么

`usbmon` 是内核里的 USB 抓包设施（相当于 USB 上的 tcpdump）。在这个项目里它回答一个
`dmesg` 回答不了的问题：**驱动有没有发、发了多少、每次是多大的矩形**。

它抓的是"驱动向 HCD 提交的请求"，所以**必须在 USB 主机那一侧跑** —— 本项目的 PUD 接在
哪台机器上，就在哪台机器上抓。

## 一、准备

1. debugfs 挂上（多数发行版默认已挂）：`mount | grep debugfs`
2. 载入 usbmon（内核 built-in 时跳过；板子上实测**需要** `modprobe`）：

```bash
sudo modprobe usbmon
ls /sys/kernel/debug/usb/usbmon/       # 出现 0s 0u 1s 1t 1u 2s 2t 2u …
```

文件名是 `<busnum><type>`：**我们要用 `u`**（`0u` = 所有总线）。同一目录里的 `t` 是 2.6.21
起弃用的旧文本格式（字段更少），`s` 不用管。详见内核自带的
`Documentation/usb/usbmon.rst`。

## 二、抓

```bash
# 1) 找到设备在哪条总线、什么地址（重启/重新枚举后都会变，别写死）
lsusb | grep 2e8a                   # -> Bus 006 Device 002

# 2) 抓 40 秒（timeout 必须套：USB 栈一卡，cat 不会返回）
timeout 40 sudo cat /sys/kernel/debug/usb/usbmon/6u > /tmp/usbmon.txt
```

> **顺序很重要：先把抓取挂上，再做那个动作。** 这一点实测踩过两次 —— 动作做完才起
> `cat`，文件里一行都没有，白等一轮。

抓的时候别做别的重活：14 秒就能有 560 行、20 万字节。

## 三、读

每行是一个事件，字段从左到右（按内核文档的说法）：

| 字段 | 例子 | 说明 |
| --- | --- | --- |
| URB tag | `ffff0000670e8900` | 同一次 URB 的 S/C 行共用一个 tag |
| 时间戳 | `1729084931` | **微秒**，相邻行相减就是间隔 |
| 事件类型 | `S` / `C` / `E` | S = 提交，C = 回调（完成），E = 提交失败 |
| 地址 | `Bo:6:002:1` | `<类型><方向>:<bus>:<dev>:<ep>` |
| 状态 | `0` / `-115` | 回调行里是 URB 的 status；**提交行里的没意义** |
| 长度 | `32` | **回调行 = 实际长度；提交行 = 请求长度** |
| 数据 | `= 58001801 …` | 只有带 `=` 的行走才有；大端 hex |

地址里的类型/方向：

| 代码 | 含义 | 本项目对应 |
| --- | --- | --- |
| `Bo` / `Bi` | bulk out / in | **`Bo:<bus>:<dev>:1` = 图像流（EP1）** |
| `Ii` | interrupt in | **`Ii:<bus>:<dev>:4` = 触摸上报（EP4，设备主动推）** |
| `Co` / `Ci` | control out / in | EP0 的厂商请求 |
| `Zo` / `Zi` | isochronous | 本驱动不用 |

**只看 C 行**：S 行的长度是"请求的"，只有 C 行才是真正发生的。

## 四、本项目常用的几条过滤

把 `6:002` 换成上一步查到的实际 `<bus>:<dev>`：

```bash
# EP1（图像）一共多少笔、多少字节
grep 'C Bo:6:002:1' /tmp/usbmon.txt | awk '{n++; b+=$6} END {print n, b}'

# 尺寸分布 —— 分带一眼看出来：几条带就是几笔
grep 'C Bo:6:002:1' /tmp/usbmon.txt | awk '{print $6}' | sort -n | uniq -c

# 相邻两笔的间隔（ms）
grep 'C Bo:6:002:1' /tmp/usbmon.txt | awk '{print $2}' \
  | awk 'NR>1 {printf "%.1f ", ($1-p)/1000} {p=$1}'

# 触摸：EP4 中断 IN，长度应该恒为 8
grep 'C Ii:6:002:4' /tmp/usbmon.txt | awk '{print $6}' | sort | uniq -c
```

## 五、实测长什么样（2026-09，RK3588 + RP2350）

四条都是这次真抓到的，可以当"正常 / 异常"的参照：

| 场景 | usbmon 里的样子 | 说明 |
| --- | --- | --- |
| 空闲、界面不动 | `C Bo:…:1` 每 600 ms 一笔，32 或 36 字节 | **fbcon 光标闪烁**的 1×25 脏矩形 —— "不交互就不花带宽" |
| 一个区域周期性刷新 | 40 组 `2818 / 3818 / 4082` 三元组 | **一个脏矩形被切成 3 条带**，每带一笔传输 |
| 手指按住拖动 | `C Ii:…:4` 全部 8 字节，节奏 `16/8/8/8` 循环 | 固件 10 ms 轮询 + 主机 `bInterval` 8 ms 的混合节奏，平均 10 ms |
| 用户态往 `/dev/fb0` 写整屏（**驱动当时还没设 `prefer_shadow_fbdev`**） | **一笔都没有** | 最有价值的一类结论：证明"根本没发"，而不是"发了没显示"（成因与修法见 [display-and-refresh.md](display-and-refresh.md)） |
| 同一个脚本，驱动设上 `prefer_shadow_fbdev` 之后 | **4368 笔 / 2.3 MB**（脚本按行写，一行一次更新） | 同一件事的**对照**：usbmon 也是验证"改好了没有"最直接的手段 |

> 最后一条的成因：fbdev 模拟层只有在驱动要求 shadow framebuffer 时才装 deferred IO
> （`drm_fbdev_use_shadow_fb()` = `prefer_shadow_fbdev` / `prefer_shadow` /
> `fb->funcs->dirty` 三者之一），本驱动一个都没满足，所以**用户态 mmap 写 `/dev/fb0`
> 不产生任何提交**。合成器（走 DRM）和 fbcon（走 fb 层的显式标脏）那两条路都正常。

## 六、和别的手段怎么配合（AGENTS.md 的"两侧对账"）

| 想知道 | 用什么 |
| --- | --- |
| 驱动发没发、发了多少、什么尺寸 | usbmon（本文） |
| 驱动有没有报错 | `dmesg`：EP1 失败会打 `EP1 transfer failed (…)`（`dev_warn_ratelimited`），连续 3 次再打一条 `dev_err` |
| 设备收到什么、丢了多少 | 设备侧计数器（gdb，见 `Pico-USB-Display/notes/debugging.md`） |

**一个推论**：驱动只在失败时打日志，所以"**dmesg 干净 + usbmon 有流量**"才等于"真的没出错"；
只看到 dmesg 干净说明不了传输发生过。

## 七、坑

- 只有 root 能读 `/sys/kernel/debug/usb/usbmon/*`。
- `cat` 会一直读下去，用完要停（`timeout` 或 Ctrl-C）。
- **不保证与总线事务精确对应**（内核文档原话）：它报的是"驱动提交给 HCD 的请求"，
  HCD 有 bug 时两者可能不一致。
- 设备号会因重新枚举而变（实测同一块板子 070 → 071 → 072）：脚本里动态发现，别写死。
- 数据不是总有：只有带 `=` 的行走才有。
- 大流量下文件涨得很快，别长时间挂着抓。

## 参考

- 内核自带文档：`Documentation/usb/usbmon.rst`（板子内核树里就有）
