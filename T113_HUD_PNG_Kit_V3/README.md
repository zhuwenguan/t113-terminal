# T113 800×600 简约科技 HUD PNG 素材包 V3

V3 为确定性矢量绘制版本，不再从 AI 预览图裁切。所有底图、线条、透明区域和动态填充层均从统一坐标参数生成，因此没有棋盘背景、抠图毛边、修补痕迹或烤死的动态值。

## 视觉规范

- 主风格：简约赛车遥测 / 工业科技 UI
- 主色：青色 `#29D9FF`
- 高光：冰白 `#ADF2FF`、白色 `#F5FAFC`
- 警告：红色 `#FF4055`
- 面板：低透明度深灰黑；1 px 灰白轮廓；2 px 青色活动标线
- 无大面积辉光、无厚重装甲框、无复杂机械装饰

## 素材清单

| 文件 | 尺寸 | 类型 | 建议位置 / 用途 |
|---|---:|---|---|
| `hud_static_frame.png` | 800×600 | 静态 | `(0,0)`，四角定位标记 |
| `left_top_panel.png` | 220×120 | 文本承载 | `(12,12)`，LAP/DELTA/TIMER/FLAG；数值区为空 |
| `right_top_panel.png` | 220×120 | 文本承载 | `(568,12)`，SOC/HV/LV/TEMP；数值区为空 |
| `rpm_arc_bg.png` | 520×170 | 静态 | `(140,430)`，24 段空槽和 RPM 固定标签 |
| `rpm_arc_fill.png` | 520×170 | 半动态 | 与 RPM 底图完全同坐标；24 段完整填充 |
| `center_gear_frame.png` | 160×110 | 文本承载 | `(320,480)`，内部完全空白 |
| `g_meter_bg.png` | 160×160 | 静态 | `(12,428)`，双圆环/十字线 |
| `g_meter_dot.png` | 32×32 | 半动态 | 按横纵向 G 值平移 |
| `pedal_bar_bg.png` | 120×180 | 静态 | `(668,408)`，三条完全空槽 |
| `pedal_bar_fill_blue.png` | 120×180 | 半动态 | 与踏板底图同坐标；左槽 10 段完整油门 fill |
| `pedal_bar_fill_red.png` | 120×180 | 半动态 | 与踏板底图同坐标；中槽 10 段完整制动 fill |
| `warning_icon_set.png` | 384×64 | sprite | 6×64 单元：温度、电池、旗语、REC、GPS、连接 |
| `small_label_plate.png` | 120×32 | 文本承载 | 通用小标题板，内部空白 |
| `number_highlight_plate.png` | 160×60 | 文本承载 | 关键数字背板，内部空白 |
| `status_pill.png` | 100×28 | 文本承载 | READY/REC/GPS/BMS/WARN，内部空白 |
| `preview_composite.png` | 800×600 | 预览 | 带示意值，仅供对照，不作为正式唯一图层 |

## 动态填充约定

### RPM

`rpm_arc_bg.png` 与 `rpm_arc_fill.png` 均为 520×170。填充层包含完整 24 段，颜色依次为青色、冰白和红色高转区。

推荐按完整段量化：

```c
ratio = clamp(rpm / rpm_max, 0.0f, 1.0f);
visible_segments = lroundf(ratio * 24.0f);
```

可以预计算 0～24 段对应的裁剪宽度，避免切开单个方格。若直接横向裁剪，源矩形和目标矩形必须保持相同 X 起点。

### 踏板

两个 fill 都是 120×180，且彩色像素已位于对应槽位：蓝色在左槽，红色在中槽。程序不需要重新计算横向位置，只需要自底向上裁剪同画布。

为了保证总是显示完整格，建议按 10 级量化，而不是切开方格：

```c
visible_segments = lroundf(clamp(value, 0.0f, 1.0f) * 10.0f);
```

第三槽暂时保持空白，可用于 regen、steering 或其他数据；确定用途后再增加对应 fill。

## T113 实现建议

- 启动时一次性解码为当前 HudSurface 使用的 ARGB8888/BGRA 字节顺序并预乘 alpha。
- PNG 像素常驻内存，禁止逐帧读取、解码或 malloc/free。
- 静态底图可在启动阶段预合成为一张 800×600 模板。
- 每帧只恢复静态模板、裁剪 RPM/踏板 fill、移动 G 点和绘制 glyph atlas 文字。
- 合成后验证 straight/premultiplied alpha，尤其检查 1 px 白色轮廓是否出现黑边。
- `preview_composite.png` 含演示值；其余动态文本承载层均为空白。
