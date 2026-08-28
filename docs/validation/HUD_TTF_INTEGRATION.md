# T113 HUD TTF 字体改造验收记录

## 结果摘要

正式 `HUD_STYLE_PNG` 路径已改为使用一个标准 TTF：

```text
assets/fonts/hud.ttf
        ↓
FT_Init_FreeType / FT_New_Face
        ↓
单一 FT_Face 常驻
        ↓
按 pixel size + Unicode codepoint 的运行时 glyph cache
        ↓
FreeType gray coverage → ARGB/RGBA straight-alpha blend
```

没有 TTF 转 `.h`、C 数组、预生成数字 PNG 或自定义字体格式。`HUD_STYLE_LEGACY` 仍保留，供 A/B 和 debug 使用；默认正式 PNG HUD 不再调用旧点阵字体。

## 资源和构建

- 字体文件：`assets/fonts/hud.ttf`
- 默认资源宏：`video/include/hud_resources.h`
- 默认路径：`assets/hud/` 和 `assets/fonts/hud.ttf`
- 可用参数：`--hud-assets PATH`、`--hud-font PATH`
- CMake 通过 `T113_FREETYPE_INCLUDE_DIR`、`T113_FREETYPE_LIBRARY` 和可选的 `T113_FREETYPE_DEPENDENCIES` 接入目标架构 FreeType；T113 SDK 需要提供与目标架构匹配的 FreeType 头文件和库。
- 安装包保持 `share/t113-terminal/assets/hud/` 与 `share/t113-terminal/assets/fonts/hud.ttf` 目录结构；若工作目录不是该资源根目录，使用 `--hud-assets` 和 `--hud-font` 指定路径。

启动时加载一次 face，并预热 16、20、24、42、72 px 五个 HUD 尺寸；退出时统一释放 glyph bitmap、`FT_Face` 和 `FT_Library`。帧循环只查找缓存并 blend。

## API

`video/include/hud_text.h` 提供：

```c
bool hud_text_draw(HudTextAtlas *atlas, HudLayer *surface,
                   int x, int baseline_y, uint32_t pixel_size,
                   HudTextColor color, HudTextAlign align,
                   const char *text);
bool hud_text_measure(HudTextAtlas *atlas, const char *text,
                      uint32_t pixel_size, HudTextMetrics *metrics);
```

`HudTextAlign` 支持 `HUD_ALIGN_LEFT`、`HUD_ALIGN_CENTER`、`HUD_ALIGN_RIGHT`。绘制坐标的 y 是 baseline；宽度使用 FreeType 的 26.6 advance，字形位置使用 `bitmap_left`/`bitmap_top`，不会用固定字符宽度估算。

glyph cache 容量为 384 项，key 只有 `pixel_size + Unicode codepoint`，每项保存 bitmap width/height/pitch、bearing、advance 和 gray alpha bitmap。请求的文字 alpha 与 FreeType coverage 相乘后进入标准 alpha over；不会将 coverage 二值化。

## 正式 HUD 迁移范围

`video/src/hud_render.c` 中由程序生成的动态文字全部经过同一个 TTF face：

- RPM、SPEED、GEAR、LAP、lap time、delta
- SOC、HV、LV、motor/battery temperature、throttle、brake
- GPS、REC、BMS、warning 状态
- 数字、负号、小数点、百分号、斜线、`°C`、`km/h`、`rpm`

PNG 负责边框、面板、RPM 弧、G meter、图标、状态框、装饰以及素材自带的静态文字。`LAP/DELTA/TIMER/FLAG`、`SOC/HV/LV/TEMP`、`THR/BRK`、`RPM` 等内置文字不再被覆盖或重绘，保持原始 PNG 像素。TTF 只绘制动态数值、动态状态和 warning 文本；程序只绘制 RPM/pedal/G 动态图形及文字。旧的 `video/src/hud_renderer.c` 手工字符路径仅由显式 `HUD_STYLE_LEGACY` 使用。

## 测试结果

主机 HUD 回归：

```text
hud_text_freetype_cache_metrics_alignment_and_antialias: PASS
hud_png_assets_alpha_layout_and_fallback:                PASS
hud_renderer_layout_and_invalid_state:                   PASS
```

新增 `tests/test_hud_text.c` 覆盖：统一 TTF 加载、Unicode `°`、非等宽测量、baseline、左右/中心对齐、gray 中间 coverage、缓存命中和重复 close。PNG 快照路径也使用同一正式 renderer。

ARMv7 T113 独立 HUD benchmark（Release，800×600，1000 warm samples）：

```text
PNG HUD wall average = 3.725 ms
PNG HUD wall P95     = 3.884 ms
PNG HUD wall P99     = 4.033 ms
resident_bytes       = 5,493,813
```

真实 `race_video` 板端链路（`/dev/video0` → MJPEG decode → G2D HUD → Cedar MJPEG encode → MP4 writer，800×600，约 10 秒）：

```text
camera/decoded/processed/encoded/written = 285/285/285/285/285
app_drop = 0, writer dropped = 0, writer full = 0
capture/decode/process/encode FPS ≈ 30.00
HUD total average = 2.333 ms
HUD total P95     = 3.800 ms
HUD total P99     = 4.600 ms
G2D healthy       = yes
MP4                = 21,933,409 bytes, 285 frames, 9.501433 s,
                     PTS monotonic, JPEG markers complete, moov at tail
```

本次完整链路在板端以 `race_video` 实际视频进程完成；一次包含全部 core/video 的单体启动受板端约 100 MB 内存环境的 OOM 约束而未作为通过项，不能据此推断字体模块失败。

重启板端并清理旧的 core/video 状态后，使用加入静态 TTF 标签清理的最终二进制复跑上述完整链路成功；此前的单体启动 OOM 属于板端低内存启动峰值，详见 [HUD_TTF_OOM_INVESTIGATION_2026-08-28.md](HUD_TTF_OOM_INVESTIGATION_2026-08-28.md)。标签清理只发生在启动时静态层合成，不进入每帧热路径。

重启后的 10 秒完整链路中，进程 RSS 在启动后约 12.2–12.4 MB 稳定，ION/DMA-BUF 在一次性分配后约 29.5 MB 稳定；停止进程后回落到显示基线约 4.9 MB。未观察到按帧持续增长、OOM 或资源池无法回收。glyph cache 预热后不会为常用文字每帧重新 rasterize，也不会每帧 malloc/free。

本次恢复素材静态文字后的板端 1 分钟复测已从 U 盘目录启动最终 ARM 二进制和 PNG/TTF 资源，并进入实际录制；测试在停止/编码器收尾阶段失去板端网络，暂时无法读取 `video.log` 和 MP4 检查结果，因此不计为通过项，待板端重启后复测。

## 800×600 截图

板端实际 renderer 生成的 800×600 BMP：

[normal_armv7.bmp](hud_ttf_snapshots/normal_armv7.bmp)

截图包含 TTF 的 `SPEED`、速度、`KM/H`、中心档位、RPM，以及左/右面板动态数值；负号、小数点、百分号和单位均可见。

## 修改文件概览

- 新增 `video/include/hud_resources.h`、`tests/test_hud_text.c`、`assets/fonts/hud.ttf`。
- 改造 `video/include/hud_text.h`、`video/src/hud_text.c` 为 FreeType face/cache/metrics/blend 模块。
- 改造 `video/src/hud_render.c`、`video/src/hud_layout.c`，将正式 PNG HUD 文本迁移到 TTF 并增加速度/RPM/中心对齐布局。
- 改造 `core/src/main.c`、`video/main.c`、`video/src/video_pipeline.c` 和 `video/include/video_pipeline.h`，集中传递资源根和字体路径。
- 更新 `CMakeLists.txt`、HUD 测试、快照和 benchmark；`video/src/hud_blend.c` 增加完全越界 coverage 的安全裁剪。
