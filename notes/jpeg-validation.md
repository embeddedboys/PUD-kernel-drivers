# ZX 硬件 JPEG 的 DRM 验证

> QEMU 直通 ZX USB HS 设备时，DRM 全屏 JPEG 能显示渐变、文字细线、照片和组合图；
> 超出约 64 KiB 的噪声图会在主机安全拒绝，下一张正常图片恢复。

## TL;DR

- 条件：2026-10-03，7.0.0-38 客户机，ZX 800×480、decoder 1、USB HS。
- 测的是显示链路正确性及超限后的恢复，没有测帧率。
- 没有修改驱动/固件容量：`USB_TRANS_MAX_SIZE=65535`，设备 `PUD_FRAME_MAX=65536`。
- 用户已确认纯蓝全屏及最终组合图与描述一致；未做逐像素比对。

## 复现

在驱动仓库，先编译再启动 QEMU；脚本只在客户机访问 framebuffer，不在宿主加载模块：

```bash
make modules
python3 tests/jpeg_patterns.py ../zx-rtt-sdk/output/validation/complex \
    --photo ../zx-rtt-sdk/application/os/jpegplayer/assets/014.jpg
make qemu PASSTHROUGH=1 \
    CMD='python3 tests/qemu_jpeg_complex.py ../zx-rtt-sdk/output/validation/complex'
```

生成目录必须位于客户机可见的工作区；客户机 `/tmp` 与宿主不共享。
依赖：pyusb、Pillow、DejaVuSans 字体和实际 ZX 设备的日志接口。
测试临时解绑 fbcon 并在退出时恢复，避免打印/控制台刷新影响计数。
脚本按当前 framebuffer 的位宽、颜色偏移和行跨度转换输入，并一次写入准备好的整帧。
用 `REQ_LOGSTAT` 的当前序号作为起点读取新 `zxdisp_stats`，防止误用历史结果；
日志查询允许秒级超时与 0.2 s 就绪轮询。日志通道失效应停止测试，不推断画面已落地。

## 复杂画面观测

以下是实际驱动编码器（RGB565、4:2:0、`JPEGE_Q_LOW`）在宿主生成的完整 JPEG 大小。
这些字节数是本组图片的观测，不是其他图片的容量保证。Pillow 均成功独立解码。

| 图片 | JPEG 大小 | QEMU + 真实设备结果 |
| --- | ---: | --- |
| 二维 RGB 渐变 | 7654 B | submitted/drawn 各增加 1 |
| 多行文字 + 横向细线 + 密集竖线 | 47139 B | submitted/drawn 各增加 1 |
| SDK `014.jpg` 缩放至全屏 | 8269 B | submitted/drawn 各增加 1 |
| 固定种子 17 的 RGB 随机噪声 | 91782 B | 主机 `-ENOSPC`，没有发送，不改变设备计数 |
| 文字 + 色条 + 照片 + 渐变组合图 | 16403 B | 噪声拒绝后仍正常接收与解码 |

本次设备 `submitted/drawn` 从 3911/3911 增至 3915/3915；
`dropped` 保持 3259，`decode_failed` 保持 0，客户机无 WARN/oops。
噪声触发 `band encode failed: -28`，下一张组合图完成解码。
测试的 `observation_ms` 包含日志请求和轮询，不能当编码时间、显示延迟或帧率。
最终组合图：顶部两行白/青文字，左侧红绿蓝白黑色条，右侧照片，底层渐变背景。

## 纯色验证记录

验证（2026-10-03）：7.0.0-38 QEMU 客户机，ZX 真设备 HS 直通，800×480、decoder 1。
解绑客户机 framebuffer console 后，红/绿/蓝三次刷新使 `submitted/drawn` 从 3774/3774 增至
3777/3777；`dropped` 保持 3236，`decode_failed` 保持 0，客户机日志无 WARN/oops/编码或发送错误。
复现：`make qemu PASSTHROUGH=1 CMD='python3 tests/qemu_jpeg_check.py'`（需要 pyusb）。
这些计数的历史基值不是验收阈值；验证的是三个更新均落地且错误计数不增加。用户已确认最后的纯蓝全屏。
未解绑 fbcon 的测试受到控制台刷新干扰，不能用于性能比较。

## 边界

- 噪声的超限拒绝是安全性验证通过，不代表所有复杂画面都能显示。
- JPEG 有损，计数和人工看屏幕均不能证明逐像素准确性。
- 重构后同步和异步 USB 路径均在 x86-64 客户机通过复杂画面回归；ARM 主机未验证。
- 同步回归 submitted/drawn 为 4013 → 4017，dropped 保持 3275；异步为 4064 → 4068，
  dropped 保持 3283。两次 decode_failed 均为 0，噪声拒绝后组合图恢复，客户机无 WARN/oops。
  异步日志明确为 `EP1 transfer path: async URB (usb_submit_urb)`。
- 编码器输入对齐与上游非对齐位流写入限制见 [encoders.md](encoders.md)。
