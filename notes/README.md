# PUD-kernel-drivers 知识库

> 维护者视角的知识库：驱动**为什么这样写**、协议**怎么约定**、验证**怎么做**。
> 仓库根 [`README.md`](../README.md) 面向使用者，讲"怎么装、怎么跑"。

## 本知识库范围

- 主机侧 USB 显示驱动 `pud`（DRM/KMS + fbdev + input）的设计与调试知识。
- 与固件仓 `Pico-USB-Display` 的协议契约（本仓 `usb-protocol.md` 为**权威定义**）。
- 不重复通用内核知识：`drm_*`、`usb_*`、`libinput` 的通用行为只在"本驱动为什么踩到它"时记录。

## 文档索引

| 文档 | 一句话内容 |
| --- | --- |
| [architecture.md](architecture.md) | 代码地图：文件职责、构建组成、运行期参数、设备参数、数据流与固件对应关系 |
| [usb-protocol.md](usb-protocol.md) | 主机↔设备 USB 厂商协议（**权威字段定义**；固件侧镜像见 `Pico-USB-Display/notes/usb-protocol.md`） |
| [display-and-refresh.md](display-and-refresh.md) | 手工 KMS 流水线、damage 局部刷新、分带规则、缓冲区、`needs_full_refresh` 兜底与残影 |
| [encoders.md](encoders.md) | 按设备 `decoder_type` 选编码器：JPEG / QOI / RLE / QOI+deflate，输入及容量限制 |
| [jpeg-validation.md](jpeg-validation.md) | ZX 全屏 JPEG 的 QEMU 真设备验证、复杂画面容量及超限恢复 |
| [input-touch.md](input-touch.md) | EP4 触摸：能力位、`report_mode`、libinput 绝对轴要求、多显示器绑定 |
| [build-and-test.md](build-and-test.md) | 构建：Makefile 设计、`PUD_USB_ASYNC`、objtree / headers / 本机三种模式 |
| [board-testing.md](board-testing.md) | 真机与 QEMU 验证：部署、期望 dmesg、检查清单、卸载坑、固件调试 |
| [kernel-api-differences.md](kernel-api-differences.md) | 6.1 / 6.12 / 7.0 的 DRM API 差异（唯一权威表） |
| [usbmon.md](usbmon.md) | 用 usbmon 看驱动实际发了什么：抓取、读法、常用过滤、实测参照 |
| [pitfalls.md](pitfalls.md) | 踩坑分类索引：DMA/内存、USB 传输、DRM、模块生命周期 |

## 维护约定

- **协议字段、常量、缓冲尺寸必须与代码一致**；改代码同步改这里，
  协议改动同时改固件仓镜像文档（见根 [`AGENTS.md`](../AGENTS.md)）。
- 结论尽量标注来源（如 `usb.c:pud_flush()`），便于核对。
- 只写**已验证**的结论；观察注明测试条件；推测显式标注"未验证"。
- 一文档一问题；新知识优先合并进既有权威文档，不建近义文档。
- 超过 ~150 行触发压缩审查，超过 ~300 行评估拆分。
- 中文叙述，命令/路径/标识符保留英文。
