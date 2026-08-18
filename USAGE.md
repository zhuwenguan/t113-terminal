# t113-terminal 使用说明

## 编译

首次构建：

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-t113.cmake
cmake --build build -j
```

后续增量编译：

```bash
cmake --build build -j
```

生成文件为 `build/t113_terminal`，目标架构是 ARM T113。

## 部署到 T113

把 `T113_HOST` 替换成开发板当前 IP：

```bash
export T113_HOST=192.168.34.238
scp build/t113_terminal root@$T113_HOST:/mnt/UDISK/t113_terminal
ssh root@$T113_HOST 'chmod +x /mnt/UDISK/t113_terminal'
```

## 启动

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

后台启动：

```bash
nohup /mnt/UDISK/t113_terminal > /tmp/t113-terminal.log 2>&1 </dev/null &
```

## 停止

建议发送 SIGINT，让程序停止 CAN、LapPro、Logger 并执行退出清理：

```bash
kill -INT $(pidof t113_terminal)
```

退出后可手动再执行一次：

```bash
sync
```

不要在程序仍运行时直接拔出 U 盘。

## Replay 回放

Replay 不启动 CAN 和 LapPro 硬件输入，从指定目录读取 CSV。目录中可放置：

- `can0.csv`
- `can1.csv`
- `lappro.csv`

也可以直接使用 Logger 生成的 `can_*.csv`、`lappro_*.csv` 文件。

运行回放：

```bash
./build/t113_terminal --replay ./replay_data --replay-speed 0
```

`--replay-speed` 含义：`0` 为最快回放，`1` 为原始时间速度，`2` 为 2 倍速。

同时查看 DEBUG：

```bash
./build/t113_terminal -v --replay ./replay_data --replay-speed 1
```

## 日志位置

程序只使用已挂载且可写的 `/mnt/usb/sda`，日志目录为：

```text
/mnt/usb/sda/log/
```

主要文件：

```text
can_YYYYMMDD_HHMMSS.csv
lappro_YYYYMMDD_HHMMSS.csv
vehicle_state_YYYYMMDD_HHMMSS.csv
lap_XXX.csv
```

文件名使用系统时间；文件内容中的 `timestamp_us` 使用 `CLOCK_MONOTONIC`。如果 U 盘未挂载或只读，程序会告警并继续运行，但不会写入日志。

查看日志：

```bash
ls -lh /mnt/usb/sda/log
tail -f /mnt/usb/sda/log/vehicle_state_*.csv
```

## 赛事模式

赛事模式不需要手动切换：

- `CIRCUIT_MODE`：检测到重复闭环 GPS 轨迹后自动进入，输出圈时间、最佳圈、Delta 和预计圈速。
- `ACCEL_MODE`：检测到静止起步和持续直线运动后自动进入，Launch Point 后 1m 开始 75m 计时。

Vehicle State 只有 CAN 必要数据和 GPS `fix_type >= 3` 同时有效时才写入 `vehicle_state.csv`。

## 快速检查

```bash
mount | grep '/mnt/usb/sda'
test -w /mnt/usb/sda && echo 'USB writable'
ip link show awlink0
ip link show awlink1
ps | grep t113_terminal
```
