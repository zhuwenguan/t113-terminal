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
- `jpeg_codec`：板端优先使用 `libjpegdecode.so`，开发机使用 libjpeg；帧缓冲预分配，摄像头输入失败可重连。
- `hud_renderer`：1280x720 基准坐标，静态层启动时生成，动态层每帧使用一次 VehicleState snapshot。
- `video_compositor`：原地 alpha blending，避免每帧复制整张 1280x720 RGBA 图像。
- `video_recorder`：对最终 HUD 帧编码为 Motion-JPEG MP4，样本索引写入临时 `.idx`，不在内存中无限累积。

## 当前板端实测

Phase 0 profiling 已加入应用层计数和阶段计时，统计每秒低频输出，并使用
`v4l2_buffer.sequence` 统计摄像头 sequence gap。T113（1.008 GHz）Release
实测结果如下：

- Mock HUD：约 31.2 FPS，HUD 约 2.1 ms，合成约 16.5 ms。
- USB MJPEG、无录像：约 9.9 FPS，3 秒收到 24 帧，sequence 丢失 45 帧，解码
  `GetFrame` 约 65.3 ms，RGB565→RGBA 约 18.5 ms，HUD 约 2.45 ms，合成约
  17.05 ms，最大 RSS 约 21 MB。
- USB MJPEG、录像：约 3.77 FPS，JPEG encode 约 192.8 ms，mux/write 约
  0.49 ms，最大 RSS 约 21 MB。

当前 `libjpegdecode.so` 的 `JpegDecoderGetFrame()` 将硬件 decode 和 picture
acquire 合并在一个接口中，应用暂时只能统计为 `decoder_get_frame`；其后的
每帧内部 decoder release 也已单独统计。Phase 0 不改变解码器或录像架构，后续
需要按板端完整 vdecoder ABI 实现常驻 decoder，并继续验证是否真正复用 VE。
