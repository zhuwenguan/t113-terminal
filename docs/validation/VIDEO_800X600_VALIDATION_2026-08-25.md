# 800×600@30 视频方案实板验证报告（2026-08-25）

## 结论

- **800×600@30：FAIL**
- **适合作为正式默认录像规格：NO**
- **T113 是否已有足够性能余量：NO**
- **DMA/ION teardown：VERIFIED（无回归）**
- **新发现的阻断项：G2D clock prepare 引用跨 session 泄漏（P0）。**
- **建议下一步：只针对该 G2D/clock P0 做一轮最小诊断/修复，然后原样重跑本测试。**

单次 fresh boot 下，800×600 原生 MJPEG 链路可以持续接近真实输入帧率，30 分钟录像也没有应用丢帧、queue full 或 DMA 泄漏。但连续 session 后 G2D clock prepare 引用没有归零，并伴随 G2D 时延显著恶化；同一 boot 的后续 10 秒 session 仅达到 24.66 FPS。该正常 start/stop 路径上的未配对内核资源引用是新 P0。按照“发现未解决 P0 立即停止”的前提，本轮在确认和复核该现象后停止，当前版本不能冻结为默认配置。

## 测试对象与约束

- 目标板：T113 实板，CPU governor `performance`，CPU 1.008 GHz。
- 测试版本：`772ec72`（生产配置）+ `74ceb1f`（离线 MP4 检查器统计增强）。
- 板端二进制：`/mnt/UDISK/t113_terminal_800x600_772ec72`。
- 视频路径：USB MJPEG capture → Cedar decode → G2D HUD → Cedar encode → bounded queue → MP4 writer。
- 同时运行：双路 CAN 流量、VehicleState、logger、lap/delta/accel、HUD、video。
- LapPro：保持 unavailable/reconnect failure 状态；正常 LapPro 实机数据链未验证，也未作为本轮通过项。
- 未改变 JPEG quality、HUD 内容、queue 策略、writer、丢帧策略或线程架构。

## 摄像头原生模式确认

| 项目 | 实测 |
|---|---:|
| 设备 | SHUNCCM USB2.0 `/dev/video0` |
| Pixel format | MJPEG |
| 协商分辨率 | 800×600 |
| Nominal FPS | 30.000 |
| UVC buffers | 3 |
| 原生采集验证 | 300 frames / 30 FPS |
| 平均 MJPEG size | 52,380 B |
| P50 / P95 / P99 | 53,834 / 53,965 / 53,989 B |
| 最大 MJPEG size | 54,030 B |

没有使用 1280×720 capture 后 resize。Cedar 返回的物理帧为 800×608、stride 800、visible crop `(0,0)-(800,600)`；pipeline 使用 logical 800×600 和原始 stride，不把 8 行 padding 当作画面。

## 最小代码变更

`772ec72` 只做运行 800×600 原生模式所需的最小调整：

1. 默认 capture/output 从 1280×720 改为 800×600，保持 30 FPS 和 3 个 UVC buffer。
2. decoder 初始化显式接收 width/height，移除 Cedar 720 高度硬编码。
3. 使用 Cedar visible crop 输出 logical dimensions，同时保留真实 stride。
4. 去掉 video/HUD 中不适用于 4:3 的 16:9 拒绝条件；HUD 继续使用现有独立 X/Y scale。
5. 增加一次性 native frame/crop 和实际 UVC buffer 数日志。

`74ceb1f` 仅让离线 MP4 inspector 输出 frame interval 的 avg/P50/P95/P99/max，不进入生产数据路径。

Host build 和 CTest：12/12 PASS；`git diff --check` PASS。

## 官方时长测试

### 吞吐与丢帧

| 时长 | Capture/encoded/written | Capture FPS | MP4 FPS | Driver sequence lost | App drop | Queue full | Queue avg/peak |
|---|---:|---:|---:|---:|---:|---:|---:|
| 10 s | 286 / 286 / 286 | 30.00 | 30.000 | 1 | 0 | 0 | 1.01 / 2 |
| 60 s | 1,778 / 1,778 / 1,778 | 29.88 | 29.880 | 7 | 0 | 0 | 1.02 / 7 |
| 5 min | 8,966 / 8,966 / 8,966 | 29.93 | 29.934 | 19 | 0 | 0 | 1.11 / 27 |
| 30 min | 53,818 / 53,818 / 53,818 | 29.91 | 29.907 | 164 | 0 | 0 | 1.06 / 25 |

30 分钟 camera sequence loss 为约 0.304%；应用没有主动丢帧，writer queue 没有 full，积压可恢复。

### Stage latency（ms）

| 时长 | Decode avg/P95/P99/max | YUV→ARGB avg/P95/P99/max | HUD avg/P95/P99/max | Blend avg/P95/P99/max | Handoff avg/P95/P99/max | Encode avg/P95/P99/max |
|---|---|---|---|---|---|---|
| 10 s | 3.600/3.7/5.2/15.990 | 3.121/3.4/4.3/27.177 | 1.638/1.9/3.4/4.695 | 4.806/5.6/6.6/7.574 | 3.002/3.5/4.4/5.182 | 2.598/2.8/3.2/4.426 |
| 60 s | 3.766/3.9/4.0/5.488 | 3.010/3.2/3.6/8.864 | 1.495/1.7/1.9/2.778 | 4.706/5.0/5.7/7.612 | 2.919/3.2/3.8/5.702 | 2.563/2.7/2.8/3.815 |
| 5 min | 3.573/3.8/3.9/7.432 | 2.985/3.2/3.5/13.743 | 1.549/1.7/2.0/4.828 | 4.693/4.9/5.5/12.810 | 2.917/3.2/3.7/8.849 | 2.556/2.6/2.8/4.594 |
| 30 min | 3.844/3.9/4.1/10.339 | 6.969/7.4/8.1/18.184 | 1.547/1.7/2.1/5.767 | 11.983/12.8/13.8/22.219 | 2.823/3.2/3.6/10.723 | 2.572/2.7/2.8/6.462 |

生产 logger 当前不保留各 stage 的原始样本或 P50，只提供 avg/P95/P99/max；因此 stage P50 明确标记为 **未采集**，没有用估算值代替。MP4 frame interval 的 P50 已由离线 inspector 验证。

30 分钟 session 的 G2D 慢状态从该 session 首秒即存在，并非录像末尾热衰减。板温最高约 49.6°C，CPU 没有降频。

### Writer 与系统资源

| 时长 | Writer avg/P95/P99/max (ms) | Throughput | App CPU（单核=100%） | System CPU（双核总容量） | Max RSS | Active fd/thread |
|---|---|---:|---:|---:|---:|---:|
| 10 s | 1.703/1/43/48.586 | 2.21 MB/s | 40.57% | 36.18% | 9,556 KB | 29 / 7 |
| 60 s | 1.474/1/12/327.917 | 2.01 MB/s | 39.93% | 21.30% | 9,916 KB | 29 / 7 |
| 5 min | 1.814/1/19/936.501 | 1.98 MB/s | 39.85% | 23.01% | 11,152 KB | 29 / 7 |
| 30 min | 1.840/1/23/993.395 | 2.11 MB/s | 73.88% | 41.78% | 11,408 KB | 29 / 7 |

30 分钟主要线程 CPU：video 64.67%、CAN 3.37%、logger 2.40%、writer 3.11%、LapPro 0.05%、snapshot 0.27%。RSS 在约 11.4 MiB 平台稳定，fd=29、thread=7，没有随时间增长。writer 约 1 秒的最坏长尾被 queue 吸收，没有反向阻塞 CAN/logger。

每个 session active 时 dma-buf/ION 为 `10 objects / 29,491,200 B`；stop 后均恢复为 `1 object / 4,915,200 B`。VE/G2D clock enable 在 stop 后回 0。

## 1 秒窗口稳定性

| 时长 | Capture FPS min/P1/P5/median | `<29 FPS` 窗口 | 最长连续 `<29 FPS` |
|---|---|---:|---:|
| 10 s | 29.84/~29.85/~29.89/29.99 | 0 | 0 s |
| 60 s | 23.18/~26.91/29.88/30.00 | 1 | 1 s |
| 5 min | 21.00/28.75/29.89/29.99 | 4 | 1 s |
| 30 min | 6.91/28.24/29.82/29.99 | 22 / 1,767 | 4 s |

平均吞吐没有掩盖持续 queue growth，但存在少量 camera/writer 长尾。30 分钟 writer 窗口最低 8 FPS、P1 26.30、P5 29.11、median 29.99，最长连续低于 29 FPS 为 2 秒；queue 随后恢复。

## MP4 / HUD 正确性

| 时长 | MP4 duration | Frames | PTS | JPEG samples | `moov`/文件尾 | 画面抽查 |
|---|---:|---:|---|---:|---|---|
| 10 s | 9.533 s | 286 | 单调 | 286/286 | 正常 | PASS |
| 60 s | 59.505 s | 1,778 | 单调 | 1,778/1,778 | 正常 | PASS |
| 5 min | 299.523 s | 8,966 | 单调 | 8,966/8,966 | 正常 | 首/中/尾 PASS |
| 30 min | 1,799.526 s | 53,818 | 单调 | 53,818/53,818 | 正常 | 首/中/尾 PASS |

30 分钟 frame interval avg/P50/P95/P99/max 为 `33.437/32.267/35.933/36.256/817.656 ms`。录像时长由真实 capture timestamp 生成，长尾和丢帧不会令时间轴加速。`.idx` 是 mux 过程中的临时 sample index，成功 finalize 后按设计写入 MP4 `moov` 并删除；最终没有独立 `.idx` 文件，MP4 内 frame count、offset/timing index 和样本数匹配。

HUD 抽查确认最终尺寸实际为 800×600，首/中/尾无明显坏帧、花屏、裁切或 HUD 缺失。

## 其他实时模块

- 双路 CAN 在测试期间持续接收；30 分钟 logger `1,035,014/1,035,014` records，drop=0、write error=0。
- VehicleState snapshot/HUD 持续运行，CAN 来源字段正常更新。
- LapPro 无数据时没有 crash、deadlock、busy loop 或高 CPU；LapPro CPU 约 0.05%。
- GPS 依赖的 lap/reference/delta 无法用当前实机 LapPro 验证。历史 replay 回归不等同于本轮 LapPro 实机 PASS。
- 视频 writer 长尾没有拖停 CAN、logger 或主程序。

## 与 1280×720 baseline 对比

对比使用 P0 修复后的现有 720p 5 分钟 baseline（`d003f8e`），不是旧泄漏版本。

| Metric | 1280×720@30 baseline | 800×600@30（5 min） | Change |
|---|---:|---:|---:|
| Capture / MP4 FPS | 27.96 | 29.93 | +1.97 (+7.0%) |
| App drop | 0 | 0 | 无变化 |
| Camera sequence loss | 6.79% | 0.21% | 明显下降 |
| Decode avg | 7.337 ms | 3.573 ms | -51.3% |
| YUV→ARGB avg | 4.879 ms | 2.985 ms | -38.8% |
| HUD avg | 2.254 ms | 1.549 ms | -31.3% |
| Blend avg | 7.790 ms | 4.693 ms | -39.8% |
| Handoff avg | 5.274 ms | 2.917 ms | -44.7% |
| Encode avg | 4.523 ms | 2.556 ms | -43.5% |
| Writer P99 | 174 ms | 19 ms | -89.1% |
| Writer max | 878 ms | 937 ms | +6.7%（存储长尾） |
| Queue peak | 24 | 27 | +3（未满且可恢复） |
| Process CPU | 49.1% | 39.9% | -18.7% |
| Max RSS | 16,296 KB | 11,152 KB | -31.6% |
| Encoded throughput | 3.21 MB/s | 1.98 MB/s | -38.3% |

fresh/短时状态的主要收益来自按像素规模下降的 decode、G2D、encode 和更小的 writer throughput；CAN、HUD control、queue 等固定开销没有显著变化。

但 30 分钟慢状态下 YUV→ARGB 为 6.969 ms、blend 为 11.983 ms，分别比 720 baseline 慢约 42.8% 和 53.8%。因此 800×600 的理论像素收益不能掩盖跨 session 的 G2D 状态退化。

## 阻止 PASS 的新 P0：G2D/clock 生命周期泄漏

测试顺序中，fresh boot 的 G2D clock 状态为：

```text
g2d enable=0 prepare=0
DMA 1 object / 4,915,200 B
```

fresh boot 后第一个 10 秒 session：

```text
active: g2d enable=1 prepare=1 rate=300 MHz
30.00 FPS
yuv_to_argb=3.053 ms, blend=4.813 ms
stop: DMA 1 object / 4,915,200 B, g2d enable=0 prepare=1
```

连续 session 后 prepare 引用累积到 5。30 分钟 session 已处于 G2D 慢状态；其后的 10 秒诊断 session 为：

```text
24.66 FPS, camera=234, lost=48, app_drop=0
yuv_to_argb=7.466 ms, blend=20.371 ms
process CPU=83.3%
g2d enable=1 prepare=5 rate=300 MHz
```

重启后 prepare 回 0，首个 10 秒 session 又恢复为 30.00 FPS 和正常 G2D 时延。这证明降速与跨 session 状态相关，而不是 800×600 本身算力必然不足，也不是 DMA buffer 泄漏或单纯热降频。现有证据尚不能证明 prepare count 是全部时延退化的唯一物理原因；底层也可能同时残留 G2D governor/rate/QoS/session state，但资源泄漏和可见后果均已成立，不影响 P0 定级。

本轮按约束没有修改该生命周期问题。发现 P0 后已停止后续性能工作。下一轮应先用最小 G2D-only open/ioctl/close 序列定位泄漏发生在 open/release 还是首个 ioctl，再查明 G2D device/context close 与 vendor clock prepare/unprepare 的配对；不得用“每轮重启”、长期持有 fd 或用户态私有 cleanup 掩盖跨进程泄漏。最小修复后至少连续 50 轮验证 prepare/enable 和 DMA 都回 session 前基线，再重复 30 分钟测试。在此之前不应继续做 memcpy、queue 或视频架构优化。

## Luna Max 独立复核

Luna Max 对 `a27c81a..74ceb1f`、实板资源快照和全部时长结果进行了只读复核，结论如下：

1. 原每 session `+12,443,648 B / 4 dma-buf` 的 DMA/ION/VE attachment teardown P0 仍为 **VERIFIED**；clock prepare 泄漏是独立资源维度，不推翻四个 compositor buffer 已释放的结论。
2. `enable_count=0` 不能写成“所有 G2D resource 已回基线”。prepare count 每轮单调 +1、跨进程保留、仅 reboot 清零，且已有实测性能后果，应定为新的 **P0**。
3. 整体 verdict 必须是 **FAIL**，不能用“每次测试前重启”降成 PASS WITH CONDITIONS。
4. `772ec72` 没有改变线程、stop 或 VE/G2D ownership/teardown；`74ceb1f` 只修改离线 inspector。最终 diff 未发现由本轮改动引入的新内存安全 P0。
5. 唯一下一方向是最小修复 `/dev/g2d` kernel/vendor clock prepare/unprepare 生命周期，再执行至少 50 轮跨 session 资源与性能验收。

## 验收判断

1. **800×600@30 是否通过：FAIL**。fresh 单 session 性能达标，但 repeated session 后实测只有 24.66 FPS。
2. **是否适合作为正式默认录像规格：NO**。当前存在可重现的跨 session G2D/clock 状态退化。
3. **T113 是否还有必要继续做视频性能优化：YES，但当前必须先修复新发现的 G2D/clock 生命周期 P0**。这不是授权进行广泛 pipeline 优化。
4. **DMA teardown：VERIFIED**。所有官方 session 和 fresh 复测 stop 后均回到 `1 / 4,915,200 B`，没有恢复原每 session 12.44 MiB 线性泄漏。
5. **推荐下一步唯一方向：对当前明确的 G2D/clock 生命周期 P0 进行一轮最小修复**；修复前不冻结默认规格，也不处理 Review P1/P2。
