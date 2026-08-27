# P0 修复后回归与稳定性验证报告（2026-08-25）

## 结论

- 被测生产版本：`6668790b841caf9d97410dfc78bd55b16ca7149c`，T113 板端二进制 SHA-256 为 `31ec9f8e415ebf91d784a1007c511537403984fd9aef098016f42d46462a036c`。其后的提交仅增加测试工具和本报告，没有修改生产路径。
- 原 P0 修复涉及的共享 `VehicleState`、replay 时钟、LapPro 新鲜度、video stop、异步 writer、文件独占创建和长圈轨迹路径，功能与压力回归均通过。
- 发布门禁结论：**不通过**。回归发现一个新的独立 P0：T113 G2D 视频会话退出后残留约 12.44 MiB ION/DMA buffer；连续启停会累积并已真实触发 OOM kill。
- 因此当前版本**不能**作为新的 stable baseline，也不应先开始 800x600 或继续 720p 性能优化。应先最小修复并验证 G2D/VE buffer teardown。
- LapPro 正常实机链：**未验证（不是 PASS）**。原因是设备当前无法开机。本轮仅验证设备缺失和持续连接失败时的安全退化。

## 环境与测试资产

| 项目 | 环境 |
|---|---|
| 主机 | WSL/x86_64，Debug、ASan+UBSan；TSan 运行时不可用 |
| 实板 | T113 ARMv7 Buildroot，约 100 MiB RAM，无 swap |
| 视频 | SHUNCCM USB MJPEG，1280x720@30；G2D + Cedar VE |
| CAN | `awlink0`/`awlink1`，500 kbit/s；loopback、error frame 和 link-down 注入 |
| 存储 | `/mnt/usb/sda`，exFAT，测试开始时约 108 GiB 可用 |
| 证据目录 | T113: `/mnt/usb/sda/p0_regression_6668790` |

为缺失的 golden/replay 和媒体检查能力新增了三个测试专用提交：

- `5fe6773`：`fwrite`/`fflush` 故障注入（delay、short、ENOSPC、EROFS、EIO）。
- `7786143`：确定性历史 GPS/CAN replay fixture 生成器。
- `0fe400c`：MJPEG MP4 box、sample、PTS、JPEG 边界完整性检查器。

## 原 P0 修复回归映射

| 原修复域 | 验证 | 结果 |
|---|---|---|
| `VehicleState`/replay/video 共享状态与时钟 | 12 个单元测试重复 100 次；ASan+UBSan 重复 20 次；同一 replay 5 次逐文件哈希 | PASS |
| LapPro 旧数据过期 | clock/freshness 单测；hci0 down；长时间无数据；退出时连接/重试中 | PASS（正常实机数据链未验证） |
| circuit/accel 并行、lap/reference/delta | race/lap/accel 单测；285 秒合成闭环 replay 5 次 | PASS |
| 长圈轨迹降采样/容量 | 20 Hz VehicleState 输出；lap/reference 单测和 285 秒 replay | PASS；900 秒实轨迹未单独跑满 |
| HMI 阻塞隔离 | HMI transport 单测；T113 长运行中 CAN/logger 持续推进 | PASS |
| video stop 请求并发 | 30 轮完整 pipeline start/record/stop；T113 10 s/60 s/5 min shutdown | stop/join 功能 PASS；DMA teardown FAIL，见新 P0 |
| recorder 独占创建、异步写入和错误隔离 | recorder 单测、30 轮录像、writer 故障矩阵、MP4 检查 | PASS |

## 基础数据链与 Replay

T113 loopback 实测解析值：`motor_rpm=1`、`torque=2 Nm`、`hv_voltage=400 V`、`hv_current=10 A`、派生功率 `4000 W`。一次 2250 帧测试全部写入 CAN CSV，0 drop、0 write error。LapPro 缺失时 VehicleState 中 CAN 字段仍更新；融合 CSV 按既定完整性规则不写入缺 GPS 的快照，没有用陈旧 GPS 补齐。

历史 GPS/CAN 合成 replay 在 T113 以 10x 可持续速率执行 5 次：

- 每次 `CAN=17103`、`LapPro=14251`、`VehicleState=5700`，总计 37054 条，0 drop/0 write error。
- CAN、LapPro、VehicleState 三个 CSV 的 SHA-256 在 5 次间分别完全一致。
- VehicleState 时间戳间隔全部为 50000 us，即稳定 20 Hz 降采样。
- 核心结果 5/5 一致：闭环 89.00 s；lap1 102.70 s；lap2/3 90.00 s；best 89.00 s；lap2/3 终点 delta +1.18 s。
- circuit/accel 独立性和 accel 50 m/75 m 结果由重复单测覆盖；仓库和板端没有现成的真实 golden replay，因此本轮不能声称与一个不存在的历史 golden 文件逐值比对。

补充观察：`--replay-speed 0` 会在 T113 上以远超实时的速率压满 1024 槽 logger 队列，5 次分别丢约 2.6k~3.5k 条；算法输出仍完全一致。这是极速 replay 的日志完整性限制，正常 10x replay 无丢失。

## LapPro 失效退化

| 场景 | 观察 | 结果 |
|---|---|---|
| 设备不存在/无法连接 | APP 保持运行；CAN/logger/HMI/video 不受影响 | PASS |
| hci0 down / Bluetooth 失败 | 每约 2 秒重试，WARN 约 0.5 Hz，非 busy loop | PASS |
| 长时间无数据/对象已初始化但无样本 | GPS 保持 invalid，融合 logger 不使用旧 GPS | PASS |
| shutdown 时连接或重试中 | stop 关闭活动 socket，约 1 秒内退出，线程/fd 回收 | PASS |
| 正常 LapPro 连接、认证、TYPE 2 数据 | 设备无法开机 | **未验证** |
| 已建立真实连接后强制关闭底层 socket | 无法建立连接，不能构造该前置状态 | **未验证** |

20 秒 hci0 down 样本中 RSS 固定约 2.0 MiB、fd 固定 10、线程固定 5，CPU 仅增加约 3 ticks。312 秒 CAN 压力中共有 157 次连接失败（约 0.503 Hz）和 313 条状态摘要（约 1 Hz），没有 busy loop 或高频刷屏。

## 1280x720@30 视频回归

| 时长 | capture/encode/write | 应用丢帧 | camera sequence lost | queue max | writer avg/P95/P99/max | CPU | max RSS |
|---|---:|---:|---:|---:|---:|---:|---:|
| 10 s | 261/261/261，27.34 FPS | 0 | 未单列 | 12 | 10.05/8/542/803 ms | 48.0% | 14.9 MiB |
| 60 s | 1399/1399/1399，23.52 FPS | 0 | 未单列 | 18 | 7.23/9/149/822 ms | 47.8% | 14.4 MiB |
| 5 min 冷启动 | 7681/7681/7681，25.65 FPS | 0 | 298 | 20 | 6.61/7/184/882 ms | 43.9% | 15.8 MiB |

5 分钟每帧平均/P95/P99：decode `6.78/6.8/6.9 ms`，YUV->ARGB `4.90/5.1/5.4 ms`，HUD `2.19/2.4/2.8 ms`，G2D blend `7.78/8.0/8.5 ms`，Cedar handoff `5.24/5.7/6.4 ms`，encode `4.52/4.6/4.7 ms`，整帧 `37.79/41.1/42.8 ms`。99.688% 帧超过 33.3 ms，说明 720p 当前仍没有稳定 30 FPS 的软件余量；本轮未做性能改动。

运行期间 RSS、fd、线程和 DMA 数量在初始化完成后保持稳定；writer 的最长 882 ms 阻塞被队列吸收，没有反向阻塞采集线程。退出后的 DMA 状态不稳定，见“新 P0”。

## MP4/PTS/文件完整性

| 文件 | frame count | duration | PTS | JPEG samples | 文件尾 | 抽样解码 |
|---|---:|---:|---|---|---|---|
| 10 s | 261 | 9.548156 s | 单调 | 261/261 | `moov` | 结构检查 PASS |
| 60 s | 1399 | 59.493856 s | 单调 | 1399/1399 | `moov` | 结构检查 PASS |
| 5 min | 7681 | 299.489100 s | 单调 | 7681/7681 | `moov` | 首/中/末帧在 T113 Cedar 上解码 PASS |

最终文件均不存在残留 `.idx`。本环境没有 ffprobe/播放器，因此“桌面播放器完整播放”没有独立执行；box/sample 全量检查和首中末实际解码已覆盖结构、索引、PTS 及 JPEG payload。掉帧 writer 测试的 61 帧文件时长 5.866 秒、36 帧文件时长 2.866 秒，没有因丢帧而加速。

## Writer/存储故障压力

| 注入 | producer | writer/queue | 结果 |
|---|---|---|---|
| 200 ms/写，6 s | 181 帧，约 30 FPS | 61 written，queue 32/32，120 recording drops | 生产侧不阻塞；MP4/PTS 完整 |
| 700 ms/写，3 s | 91 帧，约 30 FPS | 36 written，queue 32/32，55 recording drops | 生产侧不阻塞；shutdown 排空队列后完成；MP4/PTS 完整 |
| short write | 91 processed | writer 首帧失败 | 仅录像停止；APP/pipeline 继续；残缺文件不伪装为有效 MP4 |
| ENOSPC/EROFS/EIO | 每项 91 processed | writer 首帧失败 | 同上，错误被记录，`.idx` 清理 |
| fflush EIO | 91/91 written | finalize 返回失败 | 错误被记录；本次 fclose 后文件仍可完整解析 |

这些故障由 host CPU pipeline 主验证并由 Luna Max 独立复核。另在 T113 上用真实 USB camera + CPU compositor 做了同一 preload 注入，从而避免重复触发已知 G2D 泄漏：

- 200 ms/写：72 帧继续处理，58 帧写入，queue 32/32，14 个 recording-only drop；MP4 58/58 samples、PTS 单调、duration 5.381 s。
- 700 ms/写：33/33 帧写入，queue max 30，shutdown 在 writer 正在写且队列积压时排空并 rc=0；MP4 33/33 samples、PTS 单调。
- EIO：writer 首帧失败后视频继续处理到 20 帧，`recording_failed=yes`，APP rc=0；残缺文件不能通过 MP4 检查。
- 三轮前后 dma-buf 都是 5 objects/17,358,848 bytes，没有增加新残留。
- 冷启动基线再跑完整 APP（CPU compositor）+ writer EIO + 双 CAN：writer 在 1.389 秒失败后 APP 继续存活，视频继续处理到 44 帧，两路各接收 750 帧，1500 条 CAN 全部落盘且 0 drop/0 write error，rc=0；退出后 dma-buf 回到冷启动的 1 object/4,915,200 bytes。

T113 CPU compositor 只有约 8~13 FPS，此处只验证 writer 隔离和资源回收，不作为 720p 性能结果。没有执行真实拔盘、重挂载或破坏文件系统；该类硬件故障仍需用专用测试 U 盘执行。

## 并发、生命周期和 sanitizers

- 主机 30 轮完整 `pipeline start -> record -> stop`：每轮 31/31 帧，0 drop，MP4/PTS/JPEG 全部有效；测试 shell fd 4->4、线程 1->1，无残留 APP。
- T113 在 CAN/LapPro/logger/HMI 全部初始化后立即 shutdown 30 轮：30/30 rc=0、均打印 clean stop、无残留 APP。单路 CAN 在初始化期间为 down 的 cleanup 另跑 20 轮：20/20 以预期 rc=1 返回，LapPro worker/socket 和已打开的 CAN0/logger/HMI 均随进程清理，无 hang。
- T113 camera 路径不存在时连续 5 次按 1 秒退避重试；同期另一 CAN 路径继续接收并写入 300 条记录，0 drop/0 write error，APP 最终 rc=0。
- 全部 12 个 CTest：普通运行 PASS；`--repeat until-fail:100` 共 1200 次 PASS。
- ASan+UBSan：12 个测试重复 20 次，共 240 次 PASS；额外单轮复验 PASS，无 leak/UAF/UB 报告。
- T113 Debug：全部 10 个适用核心测试 PASS。数学测试需临时静态链接 libm 才能在旧 Buildroot glibc 上启动，生产 APP 已经使用静态 libm。
- TSan：WSL 上运行时在进入测试前崩溃/报 unexpected memory mapping，故 **未完成 TSan 动态竞态验证**；并发结论只能表述为代码复核和压力测试未复现竞态，不能宣称动态证明竞态已消失。
- T113 G2D 重复 start/stop 被新发现的 DMA 泄漏阻断；继续几十轮会制造已知 OOM，不再人为扩大故障。

## CAN 故障隔离

| 注入 | 故障路 | 健康路 | APP/logger | 结果 |
|---|---|---|---|---|
| CAN0 synthetic BUS-OFF error frame | `bus_off=1`, isolated | CAN1 继续 401 帧 | 401 条写入，rc=0 | PASS |
| CAN1 synthetic BUS-OFF error frame | `bus_off=1`, isolated | CAN0 继续 401 帧 | 401 条写入，rc=0 | PASS |
| CAN0 runtime link-down | epoll error 后 isolated | CAN1 继续 400 帧 | 400 条写入，rc=0 | PASS |
| 双路无数据 | 0 帧 | 0 帧 | APP/logger/video/LapPro retry 继续 | PASS |

真实无 ACK 总线在当前接线下没有可靠进入 bus-off，因此使用合法 SocketCAN `CAN_ERR_BUSOFF` error frame 注入直接覆盖应用分支。bus-off 分支关闭 fd 后，同一次 drain loop 会额外记录一次 `EBADF/read_error`，但不影响隔离或退出；记为非 P0 诊断噪声。

## 长时间资源稳定性

5 分钟视频冷启动运行段：RSS 稳定在约 15.4~15.8 MiB，fd 28、线程 7；初始化稳定后 DMA 为 10 objects/40.59 MiB，无逐帧增长；0 应用丢帧，writer queue 可恢复。退出后普通内存、fd、线程下降，但 DMA 没有回到冷启动基线。

5 分钟 CAN 压力 + logger + HUD + LapPro unavailable 实际运行 312 秒：RSS 从首个样本到结束始终为 2.084 MiB，fd 始终 10、线程始终 5，CPU ticks 1->1648（约 5.3%，其中包含 `-v` 的高频调试输出）。两路共接收并完整写入 135000 条 CAN 记录，0 drop、0 write error，queue high-watermark 38/1024；APP 在 SIGINT 前存活并以 rc=0 干净退出。LapPro raw 和融合 VehicleState 都保持 0 条，符合无 GPS 时不伪造完整融合状态的规则。

## 新发现问题

### P0：G2D 视频会话退出泄漏 VE-attached ION/DMA buffer

- 位置：`src/video_compositor.c:83` `video_ion_buffer_alloc()`、`src/video_compositor.c:156` `video_compositor_init_g2d()`、`src/video_compositor.c:269` `video_compositor_close()`。
- 实际行为：每个 1280x720 G2D 会话分配一个约 1.384 MiB YUV buffer 和三个各 3.686 MiB ARGB buffer。`video_compositor_close()` 调用 `sunxi_ion_alloc_pfree()` 和 `sunxi_ion_alloc_close()` 后，四个 dma-buf 仍显示 attached to `1c0e000.ve`。即使 camera 路径不存在、0 帧进入 pipeline，只要默认 `auto` compositor 初始化成功，退出后同一组 buffer 也会残留，因此 init/early-error path 同样受影响。
- 证据：冷启动只有显示缓冲；单个正常 5 分钟会话退出后为 5 objects/17,358,848 bytes，其中四个 VE buffer 共 12,443,648 bytes。此前顺序完成 10 s、60 s 后再启动 5 min，形成三组同尺寸残留，内核将 APP OOM-kill（rc=137）；camera-missing 测试又独立复现一组相同残留。重启后均恢复为 1 object/4,915,200 bytes。
- 后果：每次录像启停永久损失约 12.44 MiB 可用内存；在约 100 MiB、无 swap 的 T113 上少数几轮即可 OOM，影响整个 APP/系统。
- 最小修复方向：先确认 `libawion`/Cedar ABI 中由谁创建 VE IOMMU attachment，只对这四个 compositor buffer 补齐配对的 unmap/free；不要盲目重复 `VideoEncUnInit`，encoder destroy 已有自己的所有权。增加实板 teardown 断言：每轮退出后 dma-buf object/bytes 必须回到冷启动基线。
- 验证：冷启动后至少 50 轮 G2D `start-record-stop`，逐轮比较 `/sys/kernel/debug/dma_buf/bufinfo`、ION、RSS/fd/thread；注入 init failure、writer failure、shutdown 同时发生；最后再跑 5 min MP4 完整性。
- Luna Max 意见：独立确认这是新 P0，并确认泄漏尺寸与四个 compositor allocation 精确匹配；同意暂停重复 G2D 会话，反对未经 ABI 证明直接增加 encoder uninit。它还要求修复后覆盖 G2D 部分初始化失败、运行时 ioctl fatal、writer failure 与 shutdown 同时发生、recorder init failure，以及 orderly stop 与 SIGKILL/OOM 的差异。

### P1/验证限制（本轮故意不修）

1. 720p30 仍不稳定：5 分钟平均 25.65 FPS，整帧平均 37.79 ms，camera sequence lost 298；属于已有性能/链路问题，不是本轮顺手优化对象。
2. `--replay-speed 0` 会压满 logger 队列并产生调度相关的日志丢弃；10x 可持续速率完全确定且无丢失。
3. T113 测试 target 的 libm 链接与旧 glibc 不兼容；生产 target 不受影响，测试时临时静态重链。
4. finalize 阶段 `fflush` 失败时 recorder stats 正确标记 failed，但 pipeline 的 `recording_failed` 摘要字段仍可能显示 `no`；错误日志和 writer failed 字段正确。
5. bus-off 后的同轮读取会多报一次 EBADF/read_error；错误隔离语义正确。

## Baseline 与下一步

1. **P0 修复是否验证通过：** 原修复集的目标行为通过，但整体 P0 发布门禁不通过，因为发现新的 DMA/ION 生命周期 P0。
2. **是否发现新的 P0/P1：** 新 P0 1 个；P1/限制见上。
3. **能否作为 stable baseline：** 不能。
4. **LapPro 正常实机链：** 未验证，设备无法开机，不得写成 PASS。
5. **下一步优先级：** 先最小修复 G2D/VE buffer teardown，并做实板 50 轮资源回基线测试；之后再处理 Review 剩余 P1。800x600 和进一步 720p 优化都应排在该 P0 之后。

本轮没有修改生产代码，也没有处理上述 P1/P2。
