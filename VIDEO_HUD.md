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

在 T113（1.008 GHz、约 100 MB RAM）上，Release 构建的 mock HUD 为约 31 FPS，
实际 USB MJPEG 链路为约 10 FPS，0 dropped、0 decode errors；缓冲区约 10.8 MB，
进程最大 RSS 约 21 MB。实际链路平均解码约 84 ms、HUD 约 2.4 ms、合成约
16.8 ms。瓶颈是当前系统 `libjpegdecode.so` 封装每帧重新建立 Cedar JPEG 解码
上下文；需要后续按该板镜像的完整 vdecoder ABI 做常驻 decoder 适配，才能达到
1280x720@30 FPS。录像还会受到板端 JPEG 编码器吞吐限制。
