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
- `video_recorder`：对最终 HUD 帧编码为 Motion-JPEG MP4，样本索引写入临时 `.idx`，不在内存中无限累积。

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

当前无录像路径已经使用常驻 `libvdecoder.so`，因此 decode 和 picture acquire
已分别计时，且每帧不再创建/销毁 vendor decoder。由于 T113 的公开 Cedar
头文件与固件 ABI 不同，适配器复现了板端 JPEG wrapper 使用的 `0x101` codec
值、扩展 VConfig 和插件注册步骤。Phase 2 的临时 CPU YUV 合成约 58 ms，
下一阶段应切换到 T113 G2D；录像模式还受到软件 JPEG encode 约 180 ms 的限制，
待后续硬件 JPEG encoder 阶段处理。
