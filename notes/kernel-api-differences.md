# 各内核分支的 DRM API 差异

> 本驱动同时维护 `kernel-6.12`（上游）、`rk-6.1.172`（板子）与 `7.0.0-34-generic`（本机 generic，
> 名字跟着运行内核走）三个分支；同一份代码在三个内核上的写法差异集中在这里，**这是唯一权威表**。

## TL;DR

- **流水线不一样**：6.12 与 6.1 用 `drm_simple_display_pipe`；7.0 用[手工内联版](display-and-refresh.md)。
- 6.12 → 7.0 最容易漏的是 fbdev：`drm_fbdev_dma_setup()` 换成 `drm_client_setup()` **加上**
  驱动自己的 `DRM_FBDEV_DMA_DRIVER_OPS`，**少了它编得过、加载也不报错，但没有 `/dev/fb0`**。
- `pud_drm_alloc()` 失败返回 `ERR_PTR`，调用方**必须 `IS_ERR()`**，否则把错误指针当设备用（曾 oops）。

## 6.12 与 6.1

| 6.12 写法 | 6.1 写法 |
| --- | --- |
| `#include <drm/drm_fbdev_dma.h>` | `#include <drm/drm_fb_helper.h>` |
| `drm_fbdev_dma_setup()` | `drm_fbdev_generic_setup()` |
| `drm_fb_xrgb8888_to_rgb565(..., fmtcnv_state)` | 少一个 `fmtcnv_state` 参数 |
| `drm_shadow_plane_state.fmtcnv_state` | 6.1 无此字段 |
| 若干 pipe 回调宏 | `DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS` |

## 6.12 与 7.0

| 6.12 写法 | 7.0 写法 |
| --- | --- |
| `drm_fbdev_dma_setup(drm, 0)` | `drm_client_setup(drm, NULL)` **加上**驱动里的 `DRM_FBDEV_DMA_DRIVER_OPS`（即 `.fbdev_probe`） |
| `from_timer()` | `timer_container_of()` |
| `destroy_timer_on_stack()` | `timer_destroy_on_stack()` |
| `drm_dbg()` | `drm_dbg_driver()`（`drm_dbg` 在 7.0 只是它的别名） |
| `struct drm_driver.date` | 字段已删除 |
| `mode_config.prefer_shadow_fbdev` | 字段不存在；fbdev shadow 只认 `fb->funcs->dirty`（见 [display-and-refresh.md](display-and-refresh.md)） |

## 为什么 7.0 少了 `.fbdev_probe` 就没有 `/dev/fb0`

7.0 把 fbdev 模拟拆成两半：`drm_client_setup()` 只**注册客户端**，真正分配 fbdev 后备存储（以及设置
`fb_helper->funcs`）的是驱动的 `.fbdev_probe`。`drm_fb_helper_single_fb_probe()` 第一句就是：

```c
if (drm_WARN_ON(dev, !dev->driver->fbdev_probe))
	return -EINVAL;
```

**症状**：`insmod` 成功、DRM 设备注册成功、`/dev/dri/card0` 在，但**没有 `/dev/fb0`**、fbcon 不接管，
dmesg 里只有一条 `WARNING`。移植时代码能编过、probe 也不报错，很容易漏。6.1/6.12 的
`drm_fbdev_generic_setup()` / `drm_fbdev_dma_setup()` 是自己把这件事做掉的，7.0 换成了这一对组合。

## `pud_drm_alloc()` 的返回值

`pud_drm_alloc()` 用 `devm_drm_dev_alloc()`，失败时返回 `ERR_PTR(-ENOMEM)`；调用方写成 `if (!drm)`
会把错误指针当有效设备用 → `insmod` 时 SIGSEGV / oops（现场 `pud_probe+0x80`）。**必须 `IS_ERR()`**。

## 相关

- [display-and-refresh.md](display-and-refresh.md)：手工 KMS 流水线的由来与内联对应表。
- [pitfalls.md](pitfalls.md)：模块生命周期类的相关坑。
