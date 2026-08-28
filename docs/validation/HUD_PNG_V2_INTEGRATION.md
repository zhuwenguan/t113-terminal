# T113 HUD PNG V3 接入验收记录

## 结果摘要

PNG HUD 已接入现有 `VehicleState -> HUD -> G2D -> 摄像头/编码/Writer` 链路。默认样式为 `png`，旧实现保留为 `legacy`，也支持 `none`。

启动参数：

```text
--hud-style png|legacy|none
--hud-assets PATH
--no-hud                 # 等价于 --hud-style none
```

运行时 PNG 资源目录为 `assets/hud/`，内容与 `T113_HUD_PNG_Kit_V3/assets/` 原样一致；V2 原始素材包仍保留。`preview_composite.png` 只用于人工对照，没有进入正式图层。

## 代码与资源

- `video/src/hud_assets.c`: 启动时一次性加载、尺寸校验、RGBA 到目标内存格式转换、释放和常驻内存统计。
- `third_party/tiny_png.c`: 项目内轻量 RGBA8、非隔行 PNG 解码器；不依赖桌面图形框架或目标机 zlib。
- `video/src/hud_blend.c`: 边界裁剪、straight-alpha over、BGRA/ARGB 读写、源 alpha bbox 加速。
- `video/src/hud_layout.c`: 800×600 布局和 RPM/G/踏板/文字配置。
- `video/src/hud_render.c`: 业务状态格式化和动态层绘制；不读文件、不解码、不分配每帧内存。
- `video/src/hud_text.c`: 启动阶段生成固定宽度 glyph coverage atlas，动态帧只组合缓存字形。
- `tests/test_hud_png.c`: 资源尺寸、单张缺图降级、通道转换、alpha 混合、重复 close 测试。
- `tools/hud_png_snapshot.c`: 固定状态截图生成。
- `tools/hud_png_benchmark.c`: legacy/PNG 热路径基准。

## 图层与坐标

坐标均为 800×600 设计坐标，当前生产分辨率不缩放：

| 图层 | 素材 | x | y | w | h | 规则 |
|---|---|---:|---:|---:|---:|---|
| static | `hud_static_frame` | 0 | 0 | 800 | 600 | 启动时合成 |
| static | `left_top_panel` | 12 | 12 | 220 | 120 | 固定 |
| static | `right_top_panel` | 568 | 12 | 220 | 120 | 固定 |
| static | `rpm_arc_bg` | 140 | 480 | 520 | 170 | 固定；有效轮廓落在画面底部 |
| static | `center_gear_frame` | 320 | 420 | 160 | 110 | 固定；避开 RPM 有效轮廓 |
| static | `g_meter_bg` | 12 | 360 | 160 | 160 | 固定；相对原布局上移约 68 px |
| static | `pedal_bar_bg` | 668 | 408 | 120 | 180 | 固定 |
| dynamic | `rpm_arc_fill` | 140 | 480 | 520 | 170 | 24 段量化；填充经过中央档位框时分左右裁剪 |
| dynamic | pedal fills | 668 | 408 | 120 | 180 | 从底向上裁剪；油门蓝、制动红 |
| dynamic | `g_meter_dot` | — | — | 32 | 32 | 中心 `(92,440)`，Gmax=1.5，半径=52，Y 轴取反 |
| dynamic | text/status | 见代码配置 | 见代码配置 | — | — | 等宽固定步进、AA coverage |

静态层在启动时预合成。G2D/ION 路径首次遇到新的 surface 指针时复制一次 800×600 静态模板，后续帧只恢复 7 个动态区域，再直接在 caller-owned HudSurface 上绘制动态层；CPU compositor 路径使用常驻 static/dynamic buffer。PNG 不会在帧循环中打开、读取或解码。

## PNG 格式、通道和 alpha

素材实际为 RGBA8、非隔行 PNG。T113 G2D 的 `HUD_PIXEL_FORMAT_ARGB8888` 在小端内存中按 `B,G,R,A` 存储，加载阶段完成 `RGBA -> BGRA`，CPU compositor 使用 `R,G,B,A`。当前合成使用 straight alpha；`hud_blend_pixel()` 以 premultiplied 中间量计算 over，再写回 straight alpha，避免半透明边缘出现黑、白或蓝色轮廓。加载器保留了预乘开关，若底层合成契约改为 premultiplied，可在加载阶段一次性预乘。

所有 blit、源矩形、目标矩形和文字覆盖接口都做边界检查。缺失或尺寸错误的资源会打印文件名、错误原因和路径；非核心资源允许缺失，核心资源缺失时 HUD 退化为空静态层/可用文字，视频线程继续运行。

## 实际素材几何检查

解码阶段记录的非透明 alpha bbox 如下；这些值用于确认素材与文档矩形是否一致：

| 文件 | 尺寸 | alpha bbox |
|---|---:|---|
| `hud_static_frame.png` | 800×600 | (5,5)-(794,594) |
| `left_top_panel.png` | 220×120 | (0,0)-(219,119) |
| `right_top_panel.png` | 220×120 | (0,0)-(219,119) |
| `rpm_arc_bg.png` | 520×170 | (5,57)-(514,118) |
| `rpm_arc_fill.png` | 520×170 | (29,76)-(490,101) |
| `center_gear_frame.png` | 160×110 | (0,0)-(159,109) |
| `g_meter_bg.png` | 160×160 | (0,0)-(159,159) |
| `g_meter_dot.png` | 32×32 | (2,2)-(29,29) |
| `pedal_bar_bg.png` | 120×180 | (0,0)-(119,179) |
| `pedal_bar_fill_blue.png` | 120×180 | (16,36)-(35,157) |
| `pedal_bar_fill_red.png` | 120×180 | (50,36)-(69,157) |
| `warning_icon_set.png` | 384×64 | (22,14)-(368,52) |
| `small_label_plate.png` | 120×32 | (0,0)-(119,31) |
| `number_highlight_plate.png` | 160×60 | (0,0)-(159,59) |
| `status_pill.png` | 100×28 | (0,0)-(99,27) |

V3 的 `rpm_arc_fill` 实际只有中间的 (29,76)-(490,101) 有效像素，代码从加载后的 alpha bbox 取得有效 X 范围，并按 V3 的 24 段量化，而不是用整幅 170 px 高画矩形。`center_gear_frame` 与 RPM 画布按建议矩形重叠，因此高 RPM 时 fill 被分成中央框左、右两段，中央档位始终无遮挡。

V3 的踏板 fill 已位于语义对应槽：蓝色 alpha bbox 为 x=16..35，红色为 x=50..69。代码直接使用各 PNG 的 alpha bbox，不再沿用 V2 的蓝色第三槽映射；垂直方向按 alpha 边界内的 10 个完整格量化并自底向上裁剪。PNG 内容本身未做修改。

V3 warning sprite sheet 的实际单元为：`TEMPERATURE=0`、`BATTERY=1`、`FLAG=2`、`RECORDING=3`、`GPS=4`、`CONNECTION=5`。业务代码使用枚举，不直接使用数字索引；GPS 状态基于 monotonic 时间闪烁，不 sleep。

## 常驻内存

当前 15 张资源解码后的像素常驻内存为 **3,437,760 bytes**（4 BPP）：

- PNG assets: 3,437,760 bytes
- 94 个 ASCII/标点 glyph 的 8× coverage atlas: 210,560 bytes
- G2D static host template: 1,920,000 bytes
- G2D PNG HUD 自有常驻合计: **5,568,320 bytes**
- CPU compositor 另有 dynamic buffer 时: **7,488,320 bytes**

这些数值已接入 `VideoPipelineStats.memory_bytes` 和启动日志；ION/G2D surface 本身仍由原有 compositor 统计和释放。

## 截图验收

以下截图由固定 `VehicleState` 生成，均为 800×600 BMP：

- [normal.bmp](hud_png_snapshots/normal.bmp)
- [low_rpm_zero_pedal.bmp](hud_png_snapshots/low_rpm_zero_pedal.bmp)
- [high_rpm_red.bmp](hud_png_snapshots/high_rpm_red.bmp)
- [full_throttle.bmp](hud_png_snapshots/full_throttle.bmp)
- [high_brake.bmp](hud_png_snapshots/high_brake.bmp)
- [g_positive.bmp](hud_png_snapshots/g_positive.bmp)
- [g_negative.bmp](hud_png_snapshots/g_negative.bmp)
- [invalid_gps_bms.bmp](hud_png_snapshots/invalid_gps_bms.bmp)

截图重点检查了中央档位无遮挡、RPM fill 与背景同坐标、底部裁剪方向、文字边界和透明边缘。`preview_composite.png` 的实际装饰几何比需求建议矩形更窄/更内收，因此没有强行做逐像素 diff；正式 HUD 以实际 alpha bbox、建议坐标和生成截图为准。

## 测试与性能

主机测试：

```text
ctest --test-dir build-host --output-on-failure
15/15 passed
```

`test_hud_png` 覆盖 RGBA8888 和 ARGB8888 两种 surface、每张 PNG 的尺寸/通道转换、缺失目录降级和重复 close。ARMv7 板端同一测试也通过，缺失资源日志明确列出 15 个文件。

Release ARM 独立热路径基准（800×600 caller-owned ARGB surface，1000 warm samples；调试构建会明显更慢；完整原始输出已保存到 U 盘 `hud_png_benchmark.armv7.log`）：

```text
legacy: wall_avg 1.387 ms, p95 1.940 ms, p99 2.441 ms
        cold_full_copy_clear 9.358 ms, clear_avg 1.070 ms, draw_avg 0.310 ms
png:    wall_avg 2.232 ms, p95 2.906 ms, p99 3.110 ms
        cold_full_copy_clear 8.051 ms, clear_avg 0.782 ms, draw_avg 1.445 ms
        rpm 0.505 ms, pedal 0.084 ms, g 0.068 ms, text 0.775 ms
```

独立 benchmark 不包含完整 G2D 合成，但用于比较 HUD CPU 本身。首次遇到新的 ION surface 时约 7.15 ms 的完整静态模板复制只发生一次；后续使用约 0.75 ms 的动态区域恢复，没有逐帧 1.92 MB ARGB memcpy。板端真实 10 秒 G2D 录像实测：286 帧、30 FPS、0 lost/app_drop，HUD total 平均 1.931 ms（clear=0.815、draw=0.841、sync=0.266），G2D healthy=yes，Writer 队列 0 dropped/0 full，RSS 峰值约 12.8 MB，MP4 286 帧、PTS 单调、可播放。U 盘根目录短路径的 10 秒 smoke test 也通过：286 帧、Writer 无丢帧、HUD total 约 1.816 ms、输出约 17.9 MB。没有恢复约 53 ms 的 HUD upload 路径。

## 板端回归状态

- 资源/格式/缺图/重复释放：已在 ARMv7 T113 通过。
- 10 秒真实 `/dev/video0` + G2D + MP4：已通过。
- 60 秒真实 `/dev/video0` + G2D + MP4：已通过，1785 帧、30 FPS、0 lost/app_drop，HUD total 平均 1.947 ms；MP4 59.505478 秒、PTS 单调。
- 1 小时真实录像：按用户要求在约 19 分钟处停止，文件已用 sidecar index 恢复收尾到 U 盘 `/mnt/usb/sda/t113_hud_png_v2_3600.mp4`；可播放检查通过，33280 帧、1135.212711 秒、JPEG markers 33280/33280、`moov` 在文件尾。本次不作为完整 1 小时通过项；原始残留索引保留为 `t113_hud_png_v2_3600.partial.idx`。
- V3 1 分钟真实录像：U 盘目录 `/mnt/usb/sda/t113_hud_png_v3_1min_20260828/`，V3 原始 PNG、G2D 和 MP4 Writer 均正常；1786 帧、59.537433 秒、29.997934 FPS、PTS 单调、JPEG markers 1786/1786、`moov` 在文件尾、无 `.idx` 残留。最终统计 `app_drop=0`、Writer `dropped=0/full=0`，HUD 平均 2.029 ms（clear=0.781、draw=0.986、sync=0.255）。本次期间摄像头启动阶段记录 1 次 sequence gap，未造成应用帧丢弃。
- V3 录制后资源检查：无残留视频/检查进程；DMA-BUF 为 1 个、4,915,200 bytes、仅显示设备附着；ION system heap 为 1 个 buffer、4,915,200 bytes；G2D 相关时钟未保持开启；U 盘仍以 rw 挂载。
- 多次开始/停止：本轮已完成 10 秒、60 秒两次独立启动和一次 U 盘 10 秒 smoke test；本次中止暴露的 core `video-only` TERM 收尾问题已补上信号处理，后续停止会走 `video_pipeline_stop()` 和 Writer finalize。
- 完整的 APP 退出/重启及所有 dma-buf/ION/G2D clock 基线核对需长测结束后落盘；在此之前不标记为通过。

已知限制：`VehicleState` 当前没有电机温度字段，右上 TEMP 显示 `-- C`，没有伪造数据；制动压力单位尚未由车辆协议确认，因此默认按百分比，`brake_pressure_max` 已配置化；内置 AA atlas 是无 freetype 依赖的固定宽度嵌入字形，若产品需要更高字形覆盖度，下一步可替换为经许可的嵌入字体 atlas；本次长测停止前 U 盘写入出现间歇性约 1.35 秒峰值，Writer 未触发 full/drop，但导致 sidecar index 末端有缓存差异，恢复工具按已提交索引生成可播放文件。
