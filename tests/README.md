# tests/ —— 解释观察，产出 PASS/FAIL

测试**解释**工具产出的事实，对照**显式 oracle** 判定，不自己实现测量。

运行：

```bash
make test                 # 宿主机离线检查（不需要内核/模块）
tests/run_tests.py --list # 列出用例、oracle 类型与来源
tests/run_tests.py --json # 机器可读结果
tests/run_tests.py --include-kernel   # 需要已加载 pud（板子或 make qemu）
```

退出码：`0` 全 PASS / `1` 有 FAIL / `3` 有 ERROR / `5` 有 INCONCLUSIVE。
被跳过的 kernel 用例计入 `skipped`，**不等于 PASS**。

## 用例

| 用例 | 层次 | Oracle 类型 | 来源 | 需要内核 |
| --- | --- | --- | --- | --- |
| [`test_protocol_spec.py`](test_protocol_spec.py) | 离线 | SPEC | `notes/usb-protocol.md` | 否 |
| [`test_band_split.py`](test_band_split.py) | 离线 | SPEC | `notes/usb-protocol.md`、`notes/display-and-refresh.md` | 否 |
| [`test_modinfo.py`](test_modinfo.py) | 离线 | SPEC | `notes/build-and-test.md`、`notes/pitfalls.md` 4.4 | 否（无 `pud.ko` 则 INCONCLUSIVE） |
| [`test_fb_present.py`](test_fb_present.py) | 内核 | REQUIREMENT | `notes/board-testing.md` 检查清单 | 是（`make qemu`） |

`common.py` 只做两件事：调用 `tools/` 里的工具、组装结果字典；不实现设备访问。

## 约定

- 每个用例模块暴露 `TEST` / `KERNEL_REQUIRED` / `ORACLE` / `run()`，
  每项检查带 `observed` 与 `expected`。
- 硬件/环境缺失报 `INCONCLUSIVE` 或工具返回 `ENVIRONMENT_ERROR`，**不是 FAIL**。
- 不编造阈值：没有可指认来源的期望值一律不写成断言。
