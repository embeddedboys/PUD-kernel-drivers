# PUD-kernel-drivers 知识库

本目录存放驱动自身的**设计说明、协议约定、移植记录与踩坑总结**。
`README.md`（仓库根目录）面向使用者，讲"怎么装、怎么跑"；这里面向维护者，讲"为什么是这样写的"。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [architecture.md](architecture.md) | 代码地图：各文件职责、数据流、fbdev/DRM 双后端 |
| [usb-protocol.md](usb-protocol.md) | 与固件之间的 USB 厂商协议（**权威定义**，固件侧的镜像文档见 `Pico-USB-Display/notes/usb-protocol.md`） |
| [display-and-refresh.md](display-and-refresh.md) | DRM simple-pipe 移植、damage 局部刷新、QOI 分带与整屏兜底 |
| [build-and-test.md](build-and-test.md) | 交叉编译（含 6.1.118 objtree / 6.1.172 headers 两种模式）与真机验证流程 |
| [pitfalls.md](pitfalls.md) | 踩坑合集：DMA buffer 规则、`transfer buffer is on stack`、vmalloc、swiotlb、模块引用计数 |

## 相关仓库

- 固件端：`Pico-USB-Display`（RP2350 / FreeRTOS / CherryUSB），其知识库在 `Pico-USB-Display/notes/`
- QOI 编解码库：`rgb565-qoi/`（上游独立仓库），本驱动通过 `rgb565_qoi.c` / `rgb565_qoi.h` 内联集成

## 维护约定

- 文档中的**协议字段、常量值、缓冲区尺寸**必须与代码一致；改代码时请同步改动这里。
- 每处结论尽量标注来源文件（如 `usb.c:pud_flush()`），便于核对。
- 只写**已验证**的结论，推测请显式标注"未验证"。
