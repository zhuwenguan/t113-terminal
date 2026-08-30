# T113 车载 HUD 布局规范（800×600，设计取整版）

> 本版本不再机械按照参考图比例缩放，而是把参考图作为视觉参考，针对 **800×600** 直接重新排版。坐标优先使用 5 px / 10 px 网格，必要细节再使用 1 px / 2 px。

## 1. 坐标约定

```text
画布：800 × 600
原点：(0, 0) 左上角
X：向右
Y：向下
矩形：(x, y, w, h)
```

## 2. HUD 总体布局

| 区域 | X | Y | W | H |
|---|---:|---:|---:|---:|
| LAP 主信息框 | 10 | 25 | 160 | 170 |
| ACCEL EVENT | 10 | 205 | 160 | 80 |
| 底部主仪表框 | 115 | 450 | 640 | 140 |

```text
LAP      : (10, 25, 160, 170)
ACCEL    : (10, 205, 160, 80)
MAIN HUD : (115, 450, 640, 140)
```

主仪表距离画面底边 10 px。

## 3. LAP 信息框

```c
#define LAP_X 10
#define LAP_Y 25
#define LAP_W 160
#define LAP_H 170
```

推荐圆角 6 px，边框 1 px。

水平分隔线：

```text
x = 20 ... 160
y = 55
y = 115
y = 160
```

主要元素：

```text
秒表图标      (20, 32)
LAP 标题       (40, 35)
LAP TIME       (20, 65)
圈速主数字      (20, 80)
BEST           (20, 122)
BEST 右对齐     x=155
DELTA          (20, 142)
DELTA 右对齐    x=155
当前圈区域      y≈170
```

## 4. ACCEL EVENT

```c
#define ACCEL_X 10
#define ACCEL_Y 205
#define ACCEL_W 160
#define ACCEL_H 80
```

分隔线：

```text
x = 20 ... 160
y = 230
y = 255
```

内容：

```text
旗帜图标       (20, 212)
ACCEL EVENT    (42, 213)
50m            (30, 236)
50m 时间        right x=150
75m            (30, 262)
75m 时间        right x=150
```

## 5. 底部主仪表框

```c
#define MAIN_X 115
#define MAIN_Y 450
#define MAIN_W 640
#define MAIN_H 140
```

推荐圆角 8 px，边框 1 px。

### 四列分隔

竖线直接取整：

```text
x = 220
x = 435
x = 570
y = 460 ... 580
```

形成四个区域：

| 区域 | X 范围 |
|---|---|
| SPEED | 115 ~ 219 |
| THROTTLE / BRAKE / RPM | 221 ~ 434 |
| G-FORCE | 436 ~ 569 |
| VEHICLE DATA | 571 ~ 754 |

## 6. SPEED 区域

```text
SPEED 标题     (135, 465)
速度数字        (132, 485)
km/h           center x≈167, y=535
READY          center x≈167, y=558
```

推荐字号：

```text
速度：44~48 px
单位：14~16 px
READY：12~14 px
```

## 7. THROTTLE / BRAKE / MOTOR RPM

统一左边界：

```text
x = 240
```

动态数值统一右对齐：

```text
x_right = 420
```

三行位置：

| 项目 | 标题 Y | Bar Y |
|---|---:|---:|
| THROTTLE | 465 | 485 |
| BRAKE | 505 | 525 |
| MOTOR RPM | 545 | 565 |

### Pedal 分段条

建议不要再拟合参考图每个格子的原始位置，而是统一几何规则：

```c
#define PEDAL_SEGMENTS 16
#define PEDAL_SEG_W 6
#define PEDAL_SEG_GAP 2
#define PEDAL_SEG_H 10
```

总宽：

```text
16×6 + 15×2 = 126 px
```

起点：

```text
x = 240
```

每格：

```c
int sx = 240 + i * 8;
fill_rect(sx, y, 6, 10, color);
```

### MOTOR RPM 分段条

```c
#define RPM_SEGMENTS 20
#define RPM_SEG_W 4
#define RPM_SEG_GAP 2
#define RPM_SEG_H 9
```

起点：

```text
(240, 565)
```

总宽约 118 px。

## 8. G-FORCE

直接按照独立视觉中心设计：

```c
#define G_CENTER_X 502
#define G_CENTER_Y 528
#define G_RADIUS 32
```

```text
外圆：R=32
内圆：R=16
动态点：R=4
```

十字线：

```text
horizontal: (470, 528) -> (534, 528)
vertical:   (502, 496) -> (502, 560)
```

标题：

```text
center x = 502
y = 460
```

动态点：

```c
dot_x = G_CENTER_X + lat_g  / max_lat_g  * G_RADIUS;
dot_y = G_CENTER_Y - long_g / max_long_g * G_RADIUS;
```

## 9. 右侧车辆数据区

有效内容范围大致：

```text
x = 590 ... 742
```

水平分隔线：

```text
y = 490
y = 525
y = 555
```

### BATTERY

```text
图标      (592, 465)
BATTERY   (618, 466)
SOC       right≈685
Voltage   right=742
```

### POWER

```text
图标      (592, 497)
POWER     (618, 498)
Power     x≈670
Current   right=742
```

### TEMPERATURE

```text
Motor temp icon       (592, 530)
Motor temp            (614, 530)
Controller temp icon  (647, 530)
Controller temp       (669, 530)
Battery temp icon     (702, 530)
Battery temp          (724, 530)
```

### MODE

```text
图标      (592, 562)
MODE      (618, 563)
TRACK     x≈680
```

## 10. 推荐 C 布局常量

```c
#define HUD_W 800
#define HUD_H 600

/* LAP */
#define LAP_X 10
#define LAP_Y 25
#define LAP_W 160
#define LAP_H 170

/* ACCEL */
#define ACCEL_X 10
#define ACCEL_Y 205
#define ACCEL_W 160
#define ACCEL_H 80

/* MAIN */
#define MAIN_X 115
#define MAIN_Y 450
#define MAIN_W 640
#define MAIN_H 140

#define MAIN_DIV_1_X 220
#define MAIN_DIV_2_X 435
#define MAIN_DIV_3_X 570
#define MAIN_DIV_Y1 460
#define MAIN_DIV_Y2 580

/* PEDAL */
#define PEDAL_BAR_X 240
#define THROTTLE_BAR_Y 485
#define BRAKE_BAR_Y 525
#define PEDAL_SEGMENTS 16
#define PEDAL_SEG_W 6
#define PEDAL_SEG_GAP 2
#define PEDAL_SEG_H 10

/* RPM */
#define RPM_BAR_X 240
#define RPM_BAR_Y 565
#define RPM_SEGMENTS 20
#define RPM_SEG_W 4
#define RPM_SEG_GAP 2
#define RPM_SEG_H 9

/* G-FORCE */
#define G_CENTER_X 502
#define G_CENTER_Y 528
#define G_RADIUS 32

/* STATUS */
#define STATUS_LINE_X1 590
#define STATUS_LINE_X2 742
#define STATUS_LINE_1_Y 490
#define STATUS_LINE_2_Y 525
#define STATUS_LINE_3_Y 555
```

## 11. 后续布局原则

参考图只负责确定：

```text
信息层级
相对位置
整体比例感
视觉语言
```

实际 800×600 HUD 应独立设计。推荐：

```text
主布局：5 px / 10 px 网格
线条：1 px
间隙：2 / 5 / 10 px
小圆角：5~8 px
```

优先使用 `10 / 20 / 25 / 40 / 115 / 220 / 435 / 570` 这类便于理解和维护的坐标，而不是为了复刻参考图保留 `8 / 17 / 222 / 436 / 572` 之类机械缩放值。
