# T113 车载视频 HUD

## 构建

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-t113.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

主程序固定使用 `/dev/video0`、V4L2 MJPEG、1280x720、30 FPS 请求值。

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
- `hud_renderer`：1280x720 基准坐标，静态层启动时生成，动态层每帧使用一次 VehicleState snapshot。
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
```

`resource slope` 中 `cma_free_loss_kb_per_frame` 为正表示连续内存下降；
同时 `dma_buf_count_delta` 或 `fd_delta` 上升优先检查用户态生命周期。若
fd、dma-buf 和 ION allocation 均稳定而 CmaFree 仍固定下降，才进入 G2D
内核调用链审查。当前仓库没有 `video_argb_pipe` 的独立 E/F 桥接工具，
因此不把生产视频线程接入本轮诊断；E/F 应继续使用上次已验证的离线链程序，
并带上相同的监控快照。
