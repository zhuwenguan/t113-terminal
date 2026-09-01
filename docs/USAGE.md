# t113-terminal 使用说明

## 编译

主机调试构建：

```bash
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
```

主机发布构建和 Sanitizer 构建分别使用：

```bash
cmake --preset host-release
cmake --build --preset host-release --parallel

cmake --preset host-sanitize
cmake --build --preset host-sanitize --parallel
ctest --preset host-sanitize
```

T113 ARM 发布构建：

```bash
cmake --preset arm-release
cmake --build --preset arm-release --parallel
```

ARM preset 会优先使用 `T113_FREETYPE_ROOT`，否则自动检查
`$XDG_CACHE_HOME/t113-terminal/freetype-armhf` 或
`$HOME/.cache/t113-terminal/freetype-armhf`。也可以显式指定：

```bash
cmake --preset arm-release \
  -DT113_FREETYPE_ROOT=/path/to/freetype-armhf
cmake --build --preset arm-release --parallel
```

ARM 测试目标会参与交叉编译，但不能在主机上用 `ctest` 执行；板端运行时
FreeType、PNG、Zlib 从 `/usr/lib` 加载，字体使用
`/mnt/UDISK/assets/fonts/hud.ttf`。

也可以使用脚本：

```bash
scripts/build-host.sh host-debug
scripts/build-arm.sh arm-release
```

ARM 目标文件为 `out/arm-release/t113_terminal`；所有 CMake 生成物都位于
`out/` 下对应的配置目录中。

Phase 1 Web/遥测架构还生成独立的 `race_core`（当前 terminal-core 守护进程）、
`race_video`、`webd` 和 Host 测试用 `fake_state_producer`/
`fake_video_producer`。`webd` 的 ARM 二进制不依赖 Node/Python 或系统 HTTP
库。

## 部署到 T113

把 `T113_HOST` 替换成开发板当前 IP：

```bash
export T113_HOST=192.168.34.238
scp out/arm-release/t113_terminal root@$T113_HOST:/mnt/UDISK/t113_terminal
scp -r assets root@$T113_HOST:/mnt/UDISK/
ssh root@$T113_HOST 'chmod +x /mnt/UDISK/t113_terminal'
```

## 启动

完整 daemon 方式（推荐）：

```bash
cmake --install out/arm-release --prefix /opt/t113
/opt/t113/bin/race_core --battery-capacity-ah 20
/opt/t113/bin/webd --config /opt/t113/config/webd.conf \
  --web-root /opt/t113/web
```

实际部署也可以使用 `scripts/start_race.sh`，它会在确认 core 已经发布新鲜
共享状态后启动 `webd` 和现有 `race_video`。`race_video` 默认把原始摄像头
MJPEG 以 15 FPS 发布到 `/race_latest_jpeg`，`webd` 再通过
`/video/live.mjpg` 提供本地浏览器预览。`webd` 关闭或重启不会停止 core、
摄像头或录像。

浏览器查看：设备接入 T113 的 Wi-Fi 后访问 `http://<T113_IP>/`。页面中的
Onboard Video 卡片在摄像头状态为 LIVE 后自动出现；状态为 stale/offline
时会停止 `<img>` 流并显示占位提示。也可以直接检查：

```bash
curl http://<T113_IP>/api/capabilities
curl http://<T113_IP>/api/system
curl --max-time 5 -o /tmp/live.mjpg http://<T113_IP>/video/live.mjpg
```

前台启动，默认输出 INFO：

```bash
ssh root@$T113_HOST
/mnt/UDISK/t113_terminal
```

开启 DEBUG 详细信息：

```bash
/mnt/UDISK/t113_terminal -v
# 或
/mnt/UDISK/t113_terminal --verbose
```

`-v/--verbose` 会显示 CAN 逐帧、LapPro GPS、CAN Decoder 和赛事检测调试信息；不加参数时默认只显示 INFO 及以上日志。

SOC 使用电表 CAN1 的每帧电荷积分，启动时必须提供电池可用容量；初始 SOC
默认是 100%。例如 20 Ah 电池：

```bash
/mnt/UDISK/t113_terminal --battery-capacity-ah 20
```

也可用 `--soc-initial-percent <0..100>` 覆盖初始值。未提供容量时仍累计并记录
电表 Ah，但 `hv_soc_valid` 为 0，避免输出看似有效的 SOC。

后台启动：

```bash
nohup /mnt/UDISK/t113_terminal > /tmp/t113-terminal.log 2>&1 </dev/null &
```

## 停止

建议发送 SIGINT，让程序停止 CAN、LapPro、Logger 并执行退出清理：

```bash
kill -INT $(pidof t113_terminal)
```

程序退出时会先排空录像 writer、生成 MP4 `moov`、对录像文件执行
`flush`/`fsync` 并关闭文件，主程序最后再执行一次全局 `sync`。仍建议退出后手动再执行一次：

```bash
sync
```

确认进程已经退出后，还要卸载文件系统再拔盘，以清除 exFAT 的挂载状态：

```bash
umount /mnt/usb/sda
```

如果设备由系统 automount/eject 管理，则使用对应的安全弹出操作。`sync` 只保证
缓存写回，并不会代替 `umount`；在仍挂载时直接拔出 U 盘，内核仍可能报告 USB
disconnect 和 block I/O error，Windows 也可能提示检查磁盘。不要在程序仍运行时直接拔出 U 盘。

## Replay 回放

Replay 不启动 CAN 和 LapPro 硬件输入，从指定目录读取 CSV。目录中可放置：

- `can0.csv`
- `can1.csv`
- `lappro.csv`

也可以把 `--replay` 指向某个 Logger 会话目录，直接使用其中的
`can_*.csv`、`lappro_*.csv` 文件。目录中存在多组文件时，Replay 只会
选择具有相同会话后缀的一组；没有共同后缀时会拒绝混合回放。

运行回放：

```bash
./out/arm-release/t113_terminal --replay ./replay_data --replay-speed 0
```

`--replay-speed` 含义：`0` 为最快回放，`1` 为原始时间速度，`2` 为 2 倍速。

回放 SOC 时同样需要传入 `--battery-capacity-ah`；Vehicle State 日志按现有
降采样周期记录 `hv_soc_percent`、`hv_discharge_ah`、
`hv_software_discharge_ah` 和 `hv_soc_valid`，原始积分帧仍只进入 CAN raw log。

同时查看 DEBUG：

```bash
./out/arm-release/t113_terminal -v --replay ./replay_data --replay-speed 1
```

## 日志位置

程序只使用已挂载且可写的 `/mnt/usb/sda`，每次启动会创建独立会话目录：

```text
/mnt/usb/sda/log/session_YYYYMMDD_HHMMSS/
# 同一秒内重复启动时可能为 session_YYYYMMDD_HHMMSS_NNN/
```

主要文件：

```text
can_YYYYMMDD_HHMMSS.csv
lappro_YYYYMMDD_HHMMSS.csv
vehicle_state_YYYYMMDD_HHMMSS.csv
lap_XXX.csv（同名冲突时为 lap_XXX_NNN.csv）
```

会话目录和文件使用不覆盖方式创建；文件内容中的 `timestamp_us` 使用
`CLOCK_MONOTONIC`。圈轨迹由独立线程异步写入。如果 U 盘未挂载或只读，
程序会告警并继续运行，但不会写入日志。

查看日志：

```bash
ls -lh /mnt/usb/sda/log
tail -f /mnt/usb/sda/log/session_*/vehicle_state_*.csv
```

## 赛事模式

赛事模式不需要手动切换。`event_mode` 只表示赛道检测状态：

- `CIRCUIT_MODE`：检测到重复闭环 GPS 轨迹后自动进入，输出圈时间、最佳圈、Delta 和预计圈速。

直线加速是与赛道检测、圈速计量并行运行的独立 Run，不改变
`event_mode`。车辆静止并 Launch 后，从 Launch Point 后 1m 开始计时；
Vehicle State CSV 的 `accel_50m_time_s` 保存 50m 成绩，`accel_time_s`
保存 75m 成绩。75m 完成后 Run 自动回到 IDLE，等待下一次静止起步。

Vehicle State 只有 CAN 必要数据在最近 1 秒内齐全且 GPS `fix_type >= 3`
时才写入 `vehicle_state.csv`；高频 CAN 流量会优先让出日志队列容量给
LapPro 和 Vehicle State。

## 快速检查

```bash
mount | grep '/mnt/usb/sda'
test -w /mnt/usb/sda && echo 'USB writable'
ip link show awlink0
ip link show awlink1
ps | grep t113_terminal
```

## 车载视频录像

生产录像默认使用 `auto` compositor：优先初始化 G2D ARGB 链，初始化失败时
回退原 CPU YUV compositor。可用以下参数强制 A/B 模式：

```bash
./out/arm-release/t113_terminal --video-only --video-compositor g2d \
  --video-record /mnt/UDISK/onboard_g2d.mp4 --video-seconds 60
./out/arm-release/t113_terminal --video-only --video-compositor cpu \
  --video-record /mnt/UDISK/onboard_cpu.mp4 --video-seconds 60
```

`g2d` 强制模式初始化失败时只停用视频模块，CAN、LapPro、VehicleState 和
logger 仍可继续运行。运行中 G2D ioctl fatal failure 会立即熔断 G2D，停止继续
提交硬件任务；录像模式会安全停止视频线程，非录像的 `auto` 模式可切换 CPU
compositor。

生产链每秒输出 FPS、计数和阶段平均值，每 10 秒输出 RSS、CmaFree、dma-buf、
fd、ION 快照，结束时输出各阶段 avg/P95/P99/max、33.3/50/100 ms 超预算比例及
资源斜率。V4L2 输入丢帧来自 `v4l2_buffer.sequence` gap，不与应用处理失败混算。
