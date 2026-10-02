# tools/ —— 可复用工具

工具**只产出事实**：确定性、可脚本化、机器可读、不内嵌项目结论。
PASS/FAIL 判断在 [`../tests/`](../tests/README.md)。

统一形式 `工具名 <子命令> [选项]`，支持 `--help` / `--version` / `--json` / `--quiet` / `--verbose`。
退出码：`0` OK / `1` FAIL / `2` INVALID_USAGE / `3` ENVIRONMENT_ERROR / `4` TIMEOUT / `5` INCONCLUSIVE。

| 工具 | 作用 | 环境要求 | 常用命令 |
| --- | --- | --- | --- |
| [`protoctl`](protoctl) | 解析 `pud.h`、核对协议常量内部一致性、算分带像素 | 纯离线 | `protoctl fields --json`、`protoctl check`、`protoctl band --frame-max 65536` |
| [`pudctl`](pudctl) | 模块产物与运行期节点：`modinfo`/`vermagic`/`params`/`nodes`/`status` | `modinfo`（kmod）；运行期命令要模块已加载 | `pudctl vermagic --ko pud.ko`、`pudctl nodes --json` |
| [`usbmonctl`](usbmonctl) | usbmon 抓包与解析 | `capture` 要 root + debugfs + `modprobe usbmon`；`parse` 纯离线 | `usbmonctl capture -o cap.txt --bus 6`、`usbmonctl parse cap.txt --json` |
| [`fbctl`](fbctl.c) | 操作/读取 `/dev/fbN`：`info`/`fill`/`rect`/`text` | 有 `/dev/fbN` 的板子或 QEMU 客户机 | `fbctl info --json`、`fbctl text 10 10 hello` |

说明：

- `pudctl nodes` / `params` 在模块未加载时返回 `ENVIRONMENT_ERROR`，这是"环境缺失"而非测试失败。
- `usbmonctl parse` 复刻 [`notes/usbmon.md`](../notes/usbmon.md) 里的过滤，测试应当消费它的
  JSON 而不是自己解析文本。
- `fbctl` 由原 `tests/rect.c` 与 `tests/test_fb.c` 合并而来，字体在 [`font8x16.h`](font8x16.h)。
- `scripts/pud-load.sh` 仍是板子上加载/卸载的入口；`pudctl` 只读不写。

构建：`make -C tools`。
