# T113 车载视频 HUD

## 构建

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-t113.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

主程序固定使用 `/dev/video0`、原生 V4L2 MJPEG、800x600、30 FPS 请求值。

## 运行

接入车辆数据线程：

```sh
./build/t113_terminal --video
```

没有 CAN 时只测试摄像头链路：

```sh
./build/t113_terminal --video-only --video-seconds 10
./build/t113_terminal --video-only --video-frame /tmp/hud.bmp
```

HUD mock：

```sh
./build/t113_terminal --hud-demo
./build/t113_terminal --hud-demo-no-frame
```

录像必须通过最终 HUD 帧编码：

```sh
./build/t113_terminal --video-record /mnt/usb/sda/log/session_xxx/onboard.mp4
```

`--video-record` 不带路径时，程序使用当前 logger session 目录下的
`onboard.mp4`。原始 CAN、LapPro 和 VehicleState 日志仍由原 logger 独立保存。

## 模块

- `video_capture`：V4L2 MMAP 三缓冲，只接受 MJPEG。
- `jpeg_codec`：T113 板端通过 `libjpegdecode.so` 注册 JPEG 插件后，使用
  `libvdecoder.so` 的常驻解码器；开发机使用 libjpeg；帧缓冲预分配，摄像头
  输入失败可重连。旧 `libjpegdecode.so` GetFrame 路径保留为兼容回退。
- `hud_renderer`：保留 1280x720 设计基准坐标，按短边统一缩放并居中到
  800x600，避免 16:9 到 4:3 的非等比拉伸；静态层启动时生成，动态层每帧
  使用一次 VehicleState snapshot。
- `video_compositor`：RGBA mock/兼容路径原地 alpha blending；T113 原生路径
  直接在 Cedar YUV picture 上叠加 HUD，避免解码后全帧 YUV→RGBA。
- `video_encoder`：通过板端 `libvencoder.so` 的 Cedar API 创建一个常驻
  JPEG encoder，复用一个 YUV420P 输入缓存和一个 JPEG 输出缓存；分段统计
  submit、VE encode、bitstream acquire/release 和输出尺寸。
- `video_recorder`：优先将最终 native YUV HUD 帧交给硬件 encoder，写入
  Motion-JPEG MP4；硬件不可用时使用原有软件 JPEG 回退。样本索引写入临时
  `.idx`，不在内存中无限累积。

## 当前板端实测

Phase 0 profiling 已加入应用层计数和阶段计时，统计每秒低频输出，并使用
`v4l2_buffer.sequence` 统计摄像头 sequence gap。Phase 1 增加了常驻 Cedar
vdecoder 适配器，并将 buffer request、submit、decode、request picture 和
return picture 分开计时。Phase 2 保留 native YUV picture 到 HUD 合成结束，
移除了无录像主路径的全帧格式转换。T113（1.008 GHz）Release 实测结果如下：

- Mock HUD：约 31.2 FPS，HUD 约 2.1 ms，合成约 16.5 ms。
- USB MJPEG、无录像（Phase 2，10 秒）：约 15.00 FPS，142 帧成功处理、无
  decode error，Cedar decode 约 6.78 ms，格式转换 0 ms，HUD 约 1.57 ms，
  CPU YUV 合成约 58.47 ms，最大 RSS 约 15.7 MB。
- USB MJPEG、录像（Phase 2，3 秒）：约 3.66 FPS，录像边界 RGBA 转换约
  63.4 ms，JPEG encode 约 180.3 ms，mux/write 约 1.09 ms，最大 RSS 约
  19.7 MB；输出 MP4 已验证可读。
- USB MJPEG、HUD 录像（Phase 4，Release，约 5 秒）：61 帧成功编码和写入，
  约 13.67 FPS，无 decode error；Cedar decode 约 6.58 ms，HUD 约 1.67 ms，
  CPU YUV 合成约 58.99 ms，硬件 JPEG 总 encode 约 6.14 ms，其中 submit
  约 1.98 ms、VE encode 约 3.98 ms、bitstream acquire/release 约
  0.15/0.02 ms，mux/write 约 0.64 ms。JPEG 平均约 101.2 KB、最大约
  106.3 KB，最大 RSS 约 15.98 MB；独立采样约 85% 单核（双核总量约 43%）。

当前无录像路径已经使用常驻 `libvdecoder.so`，因此 decode 和 picture acquire
已分别计时，且每帧不再创建/销毁 vendor decoder。由于 T113 的公开 Cedar
头文件与固件 ABI 不同，适配器复现了板端 JPEG wrapper 使用的 `0x101` codec
值、扩展 VConfig 和插件注册步骤。Phase 2 的临时 CPU YUV 合成约 58 ms，
Phase 4 已移除录像模式的软件 JPEG encode 约 180 ms 瓶颈，当前主要瓶颈仍是
CPU YUV HUD 合成。G2D 尚未重新接入：板端没有可核对的 KickPi kernel/BSP
G2D UAPI 源码，按最接近的厂商头文件调用曾触发 `dma_buf_get failed` 和
kernel Oops，因此该阶段保持隔离并等待匹配的 BSP UAPI。

## 硬件链长期运行资源诊断

独立测试工具默认每 100 帧读取一次 `/proc/meminfo`、进程 RSS、fd 数量、
`dma_buf/bufinfo` 和 KickPi ION debugfs；退出时打印资源斜率。`0` 可关闭
周期采样，但启动和退出快照仍会保留。

部署后按以下顺序运行，先完成 2000 帧；若 `CmaFree` 持续单向下降，可提前
停止并保留输出：

```sh
# A: G2D YUV -> ARGB
/mnt/UDISK/g2d_smoke_test --operation yuv-to-argb \
  --iterations 2000 --monitor-interval 100

# B: G2D ARGB BLD_H
/mnt/UDISK/g2d_smoke_test --operation blend \
  --iterations 2000 --monitor-interval 100

# C: Cedar ARGB -> JPEG，ARGB 输入 buffer 只在初始化时分配
/mnt/UDISK/vencoder_rgb_test --format ARGB \
  --frames 5000 --monitor-interval 100

# D: G2D YUV -> ARGB -> G2D BLD_H
/mnt/UDISK/g2d_smoke_test --operation yuv-to-argb-blend \
  --iterations 2000 --monitor-interval 100

# Cedar overlay 变更的补充长测
/mnt/UDISK/vencoder_overlay_test --frames 2000 --monitor-interval 100

# E: 持久 ARGB camera + 动态 ARGB HUD -> BLD_H -> Cedar ARGB8888 -> JPEG
/mnt/UDISK/video_argb_pipeline_test --mode E --handoff copy \
  --frames 10000 --monitor-interval 100

# F: 持久 YUV420P -> BITBLT_H/NONE_H -> ARGB -> HUD -> BLD_H -> JPEG
/mnt/UDISK/video_argb_pipeline_test --mode F --handoff copy \
  --frames 10000 --monitor-interval 100
```

Phase 1 的 handoff profiling 命令（不改变生产链）：

```sh
/mnt/UDISK/video_argb_pipeline_test --mode F --handoff copy \
  --frames 1000 --benchmark --monitor-interval 100 \
  --output-prefix /mnt/UDISK/zero_copy_baseline/F_copy
```

只有日志显示 Cedar input 提供确认的 dma-buf share fd 时，才运行 direct
destination 验证：

```sh
/mnt/UDISK/video_argb_pipeline_test --mode E --handoff cedar-input \
  --frames 1000 --benchmark --monitor-interval 100
/mnt/UDISK/video_argb_pipeline_test --mode F --handoff cedar-input \
  --frames 10000 --benchmark --monitor-interval 100
```

`resource slope` 中 `cma_free_loss_kb_per_frame` 为正表示连续内存下降；
同时 `dma_buf_count_delta` 或 `fd_delta` 上升优先检查用户态生命周期。若
fd、dma-buf 和 ION allocation 均稳定而 CmaFree 仍固定下降，才进入 G2D
内核调用链审查。`video_argb_pipeline_test` 是独立离线桥接工具，不接入
`video_pipeline`、`video_compositor` 或 `video_recorder`；E/F 的所有大 buffer
在启动时分配并循环复用。默认 `--handoff copy` 向 Cedar 的持久 ARGB input
使用 CPU copy，并拆分输出 `cedar_input_acquire`、`argb_memcpy`、
`cedar_cache_flush`、`cedar_input_prepare`、`other_handoff` 和
`handoff_total`；`--handoff cedar-input` 只有在 `nShareBufFd` 被确认是
dma-buf 时才允许把该 fd 交给 G2D BLD_H，physical-address fallback 永久关闭。
F 只使用已验证的 YUV420P -> ARGB `G2D_BLT_NONE_H` 路径，不尝试 ARGB ->
YUV/NV12/NV21。

`video_argb_pipeline_test --benchmark` 会关闭逐帧 checksum、全帧非零扫描、
逐帧 JPEG 写盘和逐帧资源扫描，只保留低频资源快照、阶段计时和低频 JPEG
样本。F 的三个干净板验证结果如下（测试图案输入，不是真实 USB 摄像头）：

| 帧数 | wall/FPS | total avg/P95/max | CPU | 循环内资源斜率 |
| ---: | ---: | ---: | ---: | --- |
| 100 | 8.29 s / 12.06 | 79.432 ms / — / — | 7035.74 ms，84.9% | dma-buf/fd/RSS 增量 0/0/+16 kB |
| 1000 | 78.03 s / 12.82 | 77.660 / 78.575 / 187.101 ms | 65723.52 ms，84.2% | 0/0/0 |
| 10000 | 790.19 s / 12.66 | 78.961 / 79.752 / 188.463 ms | 667549.12 ms，84.5% | 0/0/+16 kB |

F 10000 的阶段平均为：HUD 2.408 ms、G2D YUV→ARGB 4.777 ms、G2D BLD_H
7.618 ms、ARGB handoff/copy 59.008 ms、Cedar submit 0.460 ms、硬件编码
4.486 ms、acquire/release 0.043 ms。稳态总耗时约 77～79 ms/frame，明显高于
当前约 25 ms/frame 的目标；主要瓶颈是 CPU HUD ARGB copy/flush，而不是 Cedar
编码。F 全程未调用 ARGB→YUV/NV12/NV21。

F 10000 的 JPEG 样本帧 1/1000/5000/10000 均可读，HUD 动态数值随帧号变化，
未见花屏、错色、HUD 残留或动态冻结；背景仍是离线测试图案，不能替代真实
USB 摄像头画面验证。dmesg 未见 OOM、kernel Oops、G2D timeout、VE hang 或
D state。

循环期间所有命名大 buffer 均只分配一次并复用，dma-buf、fd、RSS 保持稳定；
但退出后的 `resource shutdown snapshot` 仍暴露板端组合 teardown 残留：E 比
显示基线固定多 3 个 3.686 MB ION buffer，F 固定多 4 个（3 个 3.686 MB 加
1 个 1.384 MB），F 10000 为 `+12,443,648` bytes。工具已经记录每个 buffer
的 size、fd、虚拟/物理地址和 owner，并在释放前调用 Cedar `FREE_IOMMU_ADDR`
及 `UNMAP_DMA_BUF`；两者在该固件上返回成功，但 attachment/dma-buf 数量不
下降。Cedar 仍按既有契约只由 `VideoEncDestroy` 销毁，不显式调用
`VideoEncUnInit`。

本轮 Phase 1 的干净板 F/1000 copy baseline 为：`acquire=0.012 ms`、
`memcpy=4.732 ms`、`cache_flush=0.451 ms`、`prepare=0.003 ms`、
`other_handoff=0.011 ms`、`handoff_total=5.209 ms`；另有独立的 HUD
ARGB upload 平均 `53.276 ms`。这说明旧工具把 HUD upload 与 Cedar handoff
合并后得到的约 59 ms 不能直接作为 Cedar memcpy 成本；本轮不扩大范围去修改
HUD 视觉或生产链。

同一次 Phase 1 实测中 Cedar `GetOneAllocInputBuffer` 返回：ARGB8888、
1280x720、stride 5120、size 3686400、virtual/physical address 有效，
但 `nShareBufFd=0`，不是可用 dma-buf。工具随后将 direct 模式在任何 G2D
提交前阻断；不尝试 physical address。因而 zero-copy 尚未运行 100/1000/10000
帧，不能声称已达到 zero-copy 成功标准。Phase 1 循环期间 dma-buf/fd 稳定，
dmesg 未见 Oops、G2D timeout、L2 PageTable Invalid、VE hang 或 D-state；
但既有 Cedar/G2D teardown 残留仍为 F 的 `+12,443,648` bytes / 4 个 dma-buf。

因此当前结论是“生产链保持未切换；copy baseline 已精确拆分，但 Cedar input
没有安全可用 share fd，zero-copy 被明确阻塞”。该诊断工具未接入生产视频线程；
在未重启板端前不要重复启动它，因为每次退出会继续留下同类 Cedar attachment。

## Direct HUD ION 验证（本轮）

本轮新增 `HudSurface` caller-provided surface 接口。原有 heap RGBA renderer
接口保持兼容；direct 接口按 `pixels`、`width`、`height`、`stride_bytes` 和
`pixel_format` 写入调用者提供的 ARGB8888 surface。T113 ARGB8888 的内存字节序
按 `B,G,R,A` 写入，原有 CPU/compositor RGBA 路径仍按 `R,G,B,A` 写入。renderer
不再假定紧密 stride，也不拥有外部 surface 的生命周期。

`hud_ion_test` 的 Phase A/B 都只在初始化时分配一次 HUD ION buffer：

```text
format  ARGB8888
size    3,686,400 bytes
stride  5,120 bytes
owner   ION test harness；renderer 只借用 mmap view
```

Phase A 1000/5000 帧和 Phase B 100/1000/5000 帧均通过；Phase B 继续使用安全的
YUV420P -> ARGB `G2D_BLT_NONE_H` 和 ARGB camera + ARGB HUD -> final ARGB
`G2D_BLD_H`。代表性长测结果：

| 测试 | HUD clear avg/P95/max | HUD draw avg/P95/max | cache sync avg/P95/max | HUD total / 总帧 avg |
| --- | ---: | ---: | ---: | ---: |
| A / 5000 | 1.485 / 1.573 / 2.075 ms | 0.838 / 0.989 / 1.513 ms | 0.435 / 0.466 / 0.786 ms | 2.769 ms |
| B / 5000 | 1.484 / 1.526 / 2.628 ms | 0.862 / 1.005 / 1.958 ms | 0.434 / 0.467 / 1.142 ms | 2.780 / 15.491 ms |

Phase B 的 G2D YUV->ARGB 平均 4.705 ms、BLD_H 平均 7.985 ms，P95 分别为
4.753 ms 和 8.061 ms。HUD checksum 在动态帧间变化，HUD 与 BLD 输出样本均已
生成；实际 JPEG 抽样可解码并显示动态 HUD。Phase A/B 循环内 dma-buf count、
bytes、fd 和 RSS 均无增长，shutdown 相对测试基线无新增 dma-buf。

## F 链 Direct HUD 结果

`video_argb_pipeline_test --mode F --handoff copy` 已改为复用同一块持久 HUD
ION buffer，完全删除普通 CPU HUD buffer 到 ION HUD buffer 的全帧 upload/copy。
本次只保留 Cedar final ARGB CPU memcpy；未修改生产 `video_pipeline`、
`video_compositor`、`video_recorder`，也未运行 Cedar zero-copy、overlay 或任何
G2D YUV destination 路径。

F/10000（包含 frame 1000、10000 的低频 JPEG 抽样 checkpoint）结果：

| 阶段 | avg | P95 | max |
| --- | ---: | ---: | ---: |
| HUD clear | 1.491 ms | 1.541 ms | 2.032 ms |
| HUD draw | 0.961 ms | 1.106 ms | 1.987 ms |
| HUD cache sync | 0.436 ms | 0.473 ms | 0.772 ms |
| HUD total | 2.894 ms | 3.055 ms | 4.472 ms |
| YUV->ARGB | 4.785 ms | 4.831 ms | 11.902 ms |
| BLD_H | 7.396 ms | 7.461 ms | 14.746 ms |
| Cedar handoff total | 5.258 ms | 5.446 ms | 8.429 ms |
| Cedar JPEG encode | 4.489 ms | 4.516 ms | 5.797 ms |
| total frame | 25.450 ms | 25.691 ms | 137.451 ms |

F/10000 wall time 为 254.92 s，理论 39.23 FPS，CPU 132252 ms / 51.9%。
总帧 max 和 `other_unaccounted` max 来自低频全帧 checksum/JPEG 样本，不代表
稳态帧；稳态 P95 为 25.691 ms。命名阶段平均值与总帧平均值的差异主要来自
低频诊断成本。

F 运行期间 dma-buf count/bytes 和 fd 保持稳定，RSS 斜率为 +20 kB；direct HUD
ION buffer 在启动分配一次、退出释放一次，没有新增循环泄漏。dmesg 未发现
Oops、G2D timeout、PageTable Invalid、VE hang 或 D-state。退出后的固定
`+12,443,648 bytes / 4 buffers` 与已知 Cedar/ION teardown 残留一致，本轮未
修改或扩大该问题。

## 生产 G2D ARGB 链接入

生产 `video_pipeline` 现支持 `auto`、`g2d`、`cpu` 三种 compositor 模式。
`auto` 默认优先 G2D，初始化失败回退原 CPU YUV HUD 链；`g2d` 和 `cpu` 用于
真实摄像头 A/B。生产 G2D 路径保持单线程串行：

```text
V4L2 MJPEG -> persistent Cedar decode -> YUV420P
 -> persistent ION YUV staging -> G2D BITBLT_H/NONE_H -> ARGB camera
VehicleState single snapshot -> complete HUD direct to persistent ION ARGB
ARGB camera(src0) + HUD(src1) -> G2D BLD_H -> persistent final ARGB
 -> CPU memcpy/cache handoff -> persistent Cedar ARGB8888 JPEG
 -> timestamp-aware Motion-JPEG MP4
```

板端 decoder 的当前 `CedarVideoPicture` ABI 不提供经确认的 dma-buf fd，因此
生产 compositor 使用一块额外的持久 ION YUV staging buffer，并执行 stride-aware
CPU copy；不使用猜测的 decoder physical address。所有 YUV/ARGB 大 buffer 都只
在 pipeline 初始化时分配。没有恢复普通 CPU HUD framebuffer 到 ION 的全帧
upload；完整静态和动态 HUD 每帧直接绘入同一 ION ARGB surface。

G2D format conversion 固定使用 `G2D_BLT_NONE_H`，blend 固定使用 camera/background
作为 `src_image[0]`、HUD/overlay 作为 `src_image[1]`。代码没有 ARGB 到 YUV/NV12/
NV21 destination、Cedar overlay 或 G2D 到 Cedar zero-copy 路径。任一 G2D ioctl
失败都会将 G2D 标记为 unhealthy、关闭 G2D fd 并禁止后续提交；录像中的 fatal
failure 只停止视频模块，不结束车辆数据主程序。

MP4 sample duration 由相邻帧 monotonic timestamp 生成并压缩为 `stts` runs，实际
处理低于 30 FPS 或发生 frame gap 时不会再按固定 30 FPS 快放。对应主机回归测试
为 `video_recorder_variable_timestamps`。

板端分阶段验证命令：

```sh
/mnt/UDISK/t113_terminal --video-only --video-compositor g2d \
  --video-record /mnt/UDISK/onboard_10s.mp4 --video-seconds 10
/mnt/UDISK/t113_terminal --video-only --video-compositor g2d \
  --video-record /mnt/UDISK/onboard_60s.mp4 --video-seconds 60
/mnt/UDISK/t113_terminal --video-only --video-compositor g2d \
  --video-record /mnt/UDISK/onboard_5m.mp4 --video-seconds 300
/mnt/UDISK/t113_terminal --video-only --video-compositor g2d \
  --video-record /mnt/UDISK/onboard_15m.mp4 --video-seconds 900
```

每阶段结束后检查 MP4、HUD/VehicleState 动态变化、播放时长、`dmesg`，并保存末尾
的 latency、frame budget、sequence lost、resource slope 和 CPU 汇总。主机 mock
只用于回归，不能作为真实 USB 摄像头性能结论。

## 编码帧异步写盘队列

录像链已将 Cedar JPEG 编码与 MP4/U 盘写入解耦。采集、解码、G2D HUD 合成和
Cedar 编码仍在原视频处理线程串行执行；本轮没有把完整 capture/process 链拆成
多线程，也没有修改 G2D、HUD、Cedar 的图像格式和 buffer handoff：

```text
Video processing thread
  V4L2 -> Cedar decode -> G2D/HUD -> Cedar JPEG
       -> copy encoded JPEG + capture metadata into bounded queue

Recorder writer thread
  queue pop -> MP4 mdat + temporary sample index
            -> drain -> stts/stsz/co64/moov -> flush/close
```

只有 writer thread 可以打开、写入、seek、flush、finalize 或关闭 MP4 和临时
`.idx` 文件。producer 在 `video_recorder_init()` 完成后不再访问 mux/file 状态；
Cedar encoder 仍只由视频处理线程持有。

队列是 32 槽固定池，每槽 384 KiB，只保存编码后 JPEG 及其 `pts_us`、capture
timestamp、V4L2 sequence、frame index 和 keyframe 标志，不保存 ARGB/YUV 大图。
槽内存仅在 recorder 初始化时分配，帧循环内没有 malloc/free。producer 的
`try_push` 只执行 JPEG copy 和短临界区，不等待 writer；队列满时丢弃当前最新
编码帧并立即返回，oversize 帧同样被明确计数。writer pop 后持有槽，写完才归还，
因此统计 depth 包含 ready 和 writer in-use 槽。

MP4 sample duration 继续使用原 capture monotonic PTS。被丢帧时不重排、不补造
时间戳，下一成功帧与前一成功帧之间的真实 gap 会进入 `stts`，所以播放不会因
drop newest 而快放。停止顺序为 close queue、writer drain、join、writer 内生成
`moov` 并关闭文件，之后才释放 queue 和 encoder；最终统计从 join 后状态刷新。
正常退出时 `enqueued == written` 可用于核对排空结果；writer failure 会单独报告。

writer 的 open/write/seek/flush/finalize 任一步失败都会原子标记
`recording_failed`，关闭队列并停止后续编码入队，只结束录像；摄像头解码、G2D
合成、HUD 和车辆数据线程继续运行。运行时错误只告警一次，close 阶段也不会重复
打印 finalize 错误。

为支持长录像，`mdat` 使用标准 16-byte extended-size box（32-bit size 为 1，
随后写 64-bit largesize）；sample offset 使用 `co64`，并以 64-bit
`fseeko/ftello` 处理。这样 30 分钟约 6 GB 的单文件可以跨过 4 GiB 边界。

### Writer 指标

每秒与最终汇总分别输出 capture/decode/process/encode/enqueue/write FPS，编码、
入队和落盘 MB/s，以及：

- queue current/capacity、average、maximum、full、dropped、oversized；
- writer write latency average/P95/P99/max 和超过 50/100/200/500 ms 的次数；
- logical write calls、bytes per call、writer error count 和 `recording_failed`；
- JPEG enqueue copy 时间；该时间计入 processing total，writer I/O 时间不计入。

`frame_buffers` 理论值包含 32 x 384 KiB 的队列池。主机
`encoded_frame_queue_bounded_lifecycle` 覆盖 copy ownership、FIFO metadata、
满队列 drop、oversize、槽复用和 close/drain/wakeup；
`video_recorder_variable_timestamps` 同时验证立即 finalize 会排空队列、真实 PTS
生成 `stts`，以及 extended-size `mdat`。当前主机回归为 11/11 通过，ARM Release
构建通过。

### 真实板端验证（2026-08-25）

测试平台为 T113-S3/Linux 5.4、真实 USB MJPEG 摄像头、生产 G2D compositor、
Cedar decode/encode，录像目标为 exFAT U 盘 `/mnt/usb/sda`。每个长测前均重启，
避免把既有 Cedar/G2D teardown 固定残留带入下一轮；本轮未修改该已知 teardown
问题。

| 测试 | encoded / enqueued / written | 应用丢帧 | queue avg/max | writer max | 结果 |
| --- | ---: | ---: | ---: | ---: | --- |
| 10 s | 271 / 271 / 271 | 0 | 1.47 / 12 | 394.343 ms | 28.57 FPS，排空成功 |
| 60 s | 1704 / 1704 / 1704 | 0 | 2.66 / 22 | 763.496 ms | 28.62 FPS，排空成功 |
| 5 min | 8545 / 8545 / 8545 | 0 | 2.61 / 20 | 694.852 ms | 28.53 FPS，299.488 s |
| 15 min | 25571 / 25569 / 25569 | 2 | 1.85 / 32 | 821.873 ms | 28.43 FPS，899.532 s |
| 30 min | 51221 / 51221 / 51221 | 0 | 2.54 / 25 | 783.772 ms | 28.46 FPS，1799.525 s |

15 分钟测试在约 75 秒遇到 821.873 ms U 盘长尾，队列达到 32/32，producer 按
设计丢弃当前 2 帧后继续运行；队列随后回到稳态且余下约 14 分钟没有新增应用
丢帧。这是 drop-newest 非阻塞路径的真实板端覆盖，不是异常退出。

30 分钟 MP4 为 6,296,818,209 bytes。解析结果为 51,221 个 `stsz` sample、
51,221 个 `co64` offset、50,516 个 `stts` run；首帧 offset 40，末帧 offset
6,295,674,769、size 124,111 bytes。末帧已从大于 4 GiB 的位置直接抽取并确认是
1280x720 JPEG，HUD 合成正常。`mdat` largesize 为 `0x0000000177424448`，文件在
跨 4 GiB 后继续稳定写入约 9 分钟并正常生成 `moov`。

真实 VehicleState 回放使用
`/mnt/usb/sda/log/session_20260820_173335`：10 秒视频得到 277/277/277 帧、
9.534 秒，queue avg/max 1.82/21、应用丢帧 0；回放全程 CAN 1238、LapPro 516、
VehicleState 411 条均写入且无 drop/error。抽取中间帧确认 motor RPM、torque、
HV voltage/current/power 和 G 值随真实 snapshot 更新。

`/dev/full` 运行时故障注入在首帧触发 writer failure 后，录像停止但视频处理继续
到测试结束：10 秒处理 286 帧、30.00 FPS，`video_fault=no`、
`recording_failed=yes`、进程正常退出；最终二进制 3 秒复测同样为 30.00 FPS，
finalize 错误只记录一次。测试专用软链接和 `.idx` 已清理。

长测期间未发现 OOM、G2D/VE error、D-state、fd/dma-buf 随帧增长或新的循环泄漏。
30 分钟 max RSS 为 16,060 KiB；运行中 `MemAvailable` 约 16--17 MiB，并未随时长
持续下降。60 秒相对改造前的 23.01 FPS/1369 帧提升到 28.62 FPS/1704 帧，V4L2
sequence lost 从 414 降到 81；主要收益来自 U 盘长尾不再阻塞视频处理线程。
