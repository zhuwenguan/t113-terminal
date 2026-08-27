# `/dev/g2d` clock prepare/unprepare P0 修复与验证

日期：2026-08-26
目标板：KickPi T113，Linux 5.4.61，`CONFIG_SUNXI_G2D=y`

## 最终结论

- `/dev/g2d` prepare/unprepare 引用泄漏：**FIXED AND VERIFIED**。
- `g2d`、`bus-g2d`、`mbus-g2d` 三路 clock 在 first-open 时均为 `enable=1, prepare=1`，last-release 后均恢复为 `0/0`。
- fresh boot 后完成 100 轮 open-only、双 fd overlap、50 轮完整 APP start/stop，以及 10 s、60 s、5 min、30 min 录像；没有 prepare 引用增长、CCF warning、DMA/ION 回归或应用丢帧。
- 30 分钟录像仍出现独立的 G2D 性能慢态，最终仅 24.920 FPS。该慢态发生时 prepare 始终为 1、rate 始终为 300 MHz，因此不能再归因于本次已修复的 prepare 引用泄漏。Luna Max 将其定为阻断 800×600 stable/default 验收的高优先级 P1；本轮按约束没有处理。

## 根因

准确驱动文件：

```text
kernel/linux-5.4/drivers/char/sunxi_g2d/g2d_rcq/g2d.c
```

准确调用链：

```text
first open("/dev/g2d")
  → g2d_open()
  → g2d_clock_enable()
  → clk_prepare_enable(bus_clk)
  → clk_prepare_enable(clk)
  → clk_prepare_enable(mbus_clk)

last close / process exit
  → g2d_release()
  → g2d_clock_disable()
  → clk_disable(clk)
  → clk_disable(bus_clk)
  → clk_disable(mbus_clk)
```

`clk_prepare_enable()` 同时增加 prepare 和 enable 引用，但原 `g2d_clock_disable()` 只调用 `clk_disable()`，完全没有 `clk_unprepare()`。结果是每个 first-open/last-release session 后：

```text
enable: 1 → 0
prepare: N → N + 1
```

不是 ioctl/每帧泄漏；ioctl 只使用现有 clock，不改变引用。多 fd 使用 `para.user_cnt` 保持 first-open/last-release 语义。

系统 suspend/resume 复用相同的 `g2d_clock_disable()`/`g2d_clock_enable()`，因此原驱动在活动 fd 下每次 suspend/resume 也会留下 prepare 引用。

实板 runtime-PM 状态为：

```text
runtime_status=unsupported
runtime_enabled=disabled
runtime_usage=0
```

该 vendor driver 没有实现 runtime-PM，因此 runtime-PM 不是本问题的增量来源。

## 实际修改

仓库补丁：[kernel_patches/0001-sunxi-g2d-balance-clock-prepare.patch](kernel_patches/0001-sunxi-g2d-balance-clock-prepare.patch)

只修改 driver owner 中的三个 API：

```diff
- clk_disable(info->clk);
+ clk_disable_unprepare(info->clk);
- clk_disable(info->bus_clk);
+ clk_disable_unprepare(info->bus_clk);
- clk_disable(info->mbus_clk);
+ clk_disable_unprepare(info->mbus_clk);
```

没有修改用户态 `close(g2d_fd)`、视频 pipeline、分辨率、queue、writer、HUD、丢帧策略或 clock rate。

当前 G2D 驱动 built-in 到 KickPi boot Image，精确 KickPi SDK 不在工作区。为避免用不同 BSP 重编整个 kernel，实板 boot Image 采用与源码 patch 完全同义的最小指令补丁：

- 原 `g2d_clock_disable()` 三条 ARM `BL` 均指向 `clk_disable@c040965c`。
- 补丁后均指向 Image 中现有 `clk_disable_unprepare@c0415318`。
- 该 helper 已反汇编确认只执行 `clk_disable()` 后 `clk_unprepare()`，调用签名和 ARM ABI 一致。
- boot header、设备树、其他 kernel text 和 rootfs 均未改变。
- 原/补丁 kernel payload 只有三条 `BL` 指令的目标字段发生变化，共 6 个 byte 值不同。

Boot artifact：

| Artifact | SHA-256 |
|---|---|
| 原始 boot volume backup | `710b1f293d6bdb534fedb83bc5f95e5abf92cbdd1c8142b82c3350c6a7a6b994` |
| patched boot volume | `22d661910d2adee71ef067a48517a6c75f33a24fb6d2ed663ba878878aae5ca8` |

原始 backup 同时保存在主机 `/tmp/boot_before_g2d_clock_fix_20260826.img` 和板端 `/mnt/usb/sda/boot_before_g2d_clock_fix_20260826.img`。刷写后 `/dev/ubi0_4` 全量回读 SHA 与 patched artifact 一致，随后 fresh boot 成功。

## Luna Max 独立复核

Luna Max 独立检查了实板 kallsyms、公开 Tina 5.4 RCQ driver、源码 patch 和两个 kernel payload 的反汇编，结论：

1. 根因精确位于 `g2d_clock_disable()` 的三处错误 API，不在用户态 close、ioctl 或 runtime-PM。
2. 源码 patch 与三条 ARM `BL` 替换完全等价。
3. 正常 first-open/last-release 以及活动 fd 的 suspend/resume 可准确恢复 `0/0`。
4. 当前已验证三路 clock enable 成功路径不会因补丁产生 over-unprepare 或新 P0。
5. Vendor 原有 clock-enable partial-failure rollback/open error propagation 尚未动态注入；属于同一生命周期的 error-path 加固，但不阻断本次严格最小正常路径修复。
6. 最终实板结果足以关闭正常 first-open/last-release prepare P0，没有发现新 P0；30 分钟慢态是独立 P1，不阻断 clock hotfix 交付，但继续阻断 800×600 作为默认配置。

## Fresh boot 基线

```text
g2d       enable=0 prepare=0
bus-g2d   enable=0 prepare=0
mbus-g2d  enable=0 prepare=0
DMA/ION   1 object / 4,915,200 B
```

Boot dmesg 只有正常 G2D probe，没有 G2D/clock error。

## 隔离 clock ownership 测试

### 双 fd overlap

```text
baseline       all 0/0
fd A open      all 1/1
fd B open      all 1/1
fd A close     all 1/1
fd B close     all 0/0
```

验证 first-open/last-release 共享语义没有被改成每 fd 错误开关 clock。

### Open-only 100 轮

不执行 ioctl、ION allocation 或视频数据流，仅重复 open/close：

| Checkpoint | Active | Close 后 |
|---:|---|---|
| 1 | 三路 `1/1` | 三路 `0/0` |
| 25 | 三路 `1/1` | 三路 `0/0` |
| 50 | 三路 `1/1` | 三路 `0/0` |
| 75 | 三路 `1/1` | 三路 `0/0` |
| 100 | 三路 `1/1` | 三路 `0/0` |

没有 CCF warning 或 G2D error。

## 完整 APP 50 轮 start/stop

每轮使用完整数据流：双 CAN、VehicleState、logger、HUD、G2D、encode、writer；LapPro 保持 unavailable。每轮实际完成 15–17 帧视频后正常 stop。

原始逐轮证据：

```text
/mnt/usb/sda/g2d_clock_fix_20260826/rounds_50.tsv
```

汇总：

```text
rounds=50
bad_rounds=0
frames=796
FPS min/avg/max=29.91/29.991/30.08
active clocks: every round all 1/1
idle clocks: every round all 0/0
DMA/ION after stop: every round 1 / 4,915,200 B
app_drop=0
clock/G2D warnings=0
```

第 1、10、25、50 轮均得到完全相同的资源结果。

## 指定时长回归

| Test | Frames | MP4 FPS | V4L2 sequence lost | App drop / queue full | G2D YUV→ARGB / blend avg | Stop clocks | Stop DMA/ION |
|---|---:|---:|---:|---:|---:|---|---|
| 10 s | 286 | 30.000 | 1 | 0 / 0 | 3.064 / 4.825 ms | 三路 `0/0` | `1 / 4,915,200 B` |
| 60 s | 1,785 | 29.997 | 0 | 0 / 0 | 2.983 / 4.730 ms | 三路 `0/0` | `1 / 4,915,200 B` |
| 5 min | 8,931 | 29.817 | 54 | 0 / 0 | 2.970 / 4.712 ms | 三路 `0/0` | `1 / 4,915,200 B` |
| 30 min | 44,844 | 24.920 | 9,135 | 0 / 0 | 10.672 / 16.709 ms | 三路 `0/0` | `1 / 4,915,200 B` |

所有文件均通过 inspector：

- MP4 frame count 与应用 written count 一致；
- PTS 单调；
- 每帧 JPEG marker 完整；
- duration 与真实 timestamp 一致；
- 文件尾 `moov` 正常。

30 分钟文件：

```text
bytes=3,206,816,829
frames=44,844
duration=1,799.505756 s
fps=24.920176
PTS monotonic=yes
JPEG markers=44,844/44,844
frame interval avg/P50/P95/P99/max=40.128/39.856/42.322/45.111/298.000 ms
```

30 分钟 active 期间：

```text
g2d/bus-g2d/mbus-g2d = 1/1
DMA/ION = 10 / 29,491,200 B
RSS max = 10,620 KB
queue avg/peak = 1.02/18
```

最终 last release 后：

```text
g2d/bus-g2d/mbus-g2d = 0/0
DMA/ION = 1 / 4,915,200 B
process RSS/fd/thread = 0/0/0
```

整个 fresh-boot 验证期间 dmesg 中 CCF/G2D warning、already-unprepared、unbalanced clock、G2D timeout 均为 0。

## 尚未动态验证的边界

- 三路 clock 中途 enable 失败：当前实板没有安全 fault injection；只完成源码审计，没有伪写 PASS。
- 活动 fd 的 system suspend/resume：板端没有已确认的安全唤醒路径，本轮没有冒险执行；共享 disable 函数已由源码和指令路径确认使用配对 helper。
- Malformed DT/probe failure：不是本次正常 session prepare 泄漏根因，未扩大修改范围。

## 独立性能问题

prepare 引用修复后，前 50 轮完整 APP 和随后 10 s、60 s、5 min 均保持正常约 3/4.7 ms G2D 时延。但 30 分钟 session 从约 38 秒开始进入慢态：

```text
prepare remains 1
g2d rate remains 300 MHz
yuv_to_argb ≈ 10–13 ms
blend ≈ 16–19 ms
output ≈ 22–25 FPS
```

该现象证明之前观察到的 G2D 性能污染还有一个与 prepare count 无关的根因。它没有造成 prepare/DMA/ION 泄漏、app drop 或 queue accumulation，但会继续阻断“800×600@30 稳定默认配置”的验收。

30 分钟 `camera_sequence_lost=9,135`，约占 camera sequence 总量的 16.9%。这是 G2D 处理低于输入速率后 UVC/V4L2 侧表现出的严重实时性能损失，强化 P1 判定；但 MP4 使用真实 capture timestamp、PTS 单调、文件完整，且没有 crash、OOM、资源泄漏或数据结构损坏，因此当前证据不支持把它升级为新 P0。

本轮严格遵守范围，没有修改 clock rate、governor、G2D ioctl、buffer、pipeline 或用户态策略，也没有把该问题与 prepare P0 混在一起顺手处理。
