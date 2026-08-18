# T113 Terminal 项目协作说明

## 项目概况

- 本项目面向全志 T113 平台，目标系统为 Linux。
- 本地开发环境运行在 WSL 中。
- 项目使用 `arm-linux-gnueabihf-gcc` 进行 ARM 交叉编译。
- CMake 工具链文件为 `toolchain-t113.cmake`。
- 程序入口为 `src/main.c`，构建目标名为 `t113_terminal`。

## 当前开发阶段：双 CAN + LapPro 统一 APP

- 当前程序同时监听 `awlink0` 和 `awlink1`。
- 两路接口各使用一个非阻塞 SocketCAN RAW socket，由同一个 `epoll` 实例和线程统一监听。
- `bus = 0` 对应 `awlink0`，`bus = 1` 对应 `awlink1`。
- 每帧统一转换为包含 `timestamp_us`、`bus`、`id`、`flags`、`dlc` 和 `data[8]` 的 `CanMessage`。
- 时间戳使用 `CLOCK_MONOTONIC`，单位为微秒。
- CAN 原始帧、LapPro 解析样本和 Vehicle State 统一进入固定容量 `LogRecord` 队列；三类数据分别写入独立文件。
- LapPro 日志同时保存设备/GPS 真实时间和 `CLOCK_MONOTONIC` 的 `timestamp_us`；Vehicle State 只有 CAN 必需信号齐全且 LapPro `fix_type >= 3` 时才记录融合快照。
- 第二阶段增加每路 `rx_frames`、错误帧、非法帧、读取错误、epoll 错误和 bus-off 统计。
- SocketCAN 已启用 error frame 过滤；检测到 `CAN_ERR_BUSOFF` 时标记接口失败并停止接收，避免静默运行。
- 程序退出时打印两路 CAN 的统计汇总。
- logger 使用一个线程和固定容量 `LogRecord` 队列；CAN/LapPro/Vehicle State producer 只入队，不直接执行文件写入。
- logger 每最多 32 条记录或首条记录起 1 秒批量写入一次 CSV，不对每条记录执行 `fsync`。
- 每次运行按系统本地时间生成 `/mnt/usb/sda/log/can_YYYYMMDD_HHMMSS.csv`、`lappro_YYYYMMDD_HHMMSS.csv` 和 `vehicle_state_YYYYMMDD_HHMMSS.csv` 文件；文件内记录时间戳仍使用 `CLOCK_MONOTONIC`，并统计入队、写入、丢弃和写错误数量。
- Vehicle State 框架包含车速、姿态/GPS、加速度、圈速/赛道字段，以及 `motor_rpm`、`torque_nm`、`hv_voltage_v`、`hv_current_a`、`hv_power_w`、`hv_soc_percent` 和 `lv_voltage_v`；当前 CAN Decoder 已接入 `0x121`、`0x201`、`0x123`，`hv_power_w` 由高压电压和电流计算，圈速字段及其他未提供字段保持预留状态。
- `race_event` 在 `UNKNOWN` 状态同时维护闭环和直线加速检测器，自动锁定 `CIRCUIT_MODE` 或 `ACCEL_MODE`，不依赖人工模式参数；多个闭环候选点同时匹配时，按历史时间更早者作为维修区汇入赛道点。
- `gps_track` 使用固定容量缓存将有效 GPS 转换为局部 XY，并保留时间、速度、航向和累计距离；缓存满后仅保留最近一半样本。
- `lap_timer` 从重复 GPS 轨迹学习临时赛道，只在闭环点沿赛道前进方向的前 25% 范围内选择最直区域作为虚拟起终点；当前圈从车辆首次有效运动开始计时，首次识别后等待真实虚拟起点通过，再输出当前圈、最佳圈、Delta 和圈轨迹。
- `accel_test` 检测静止起步和持续直线运动，从 Launch Point 偏移 1m 后开始 75m 计时，并输出最大纵向加速度。
- logger 不可用时只输出告警，CAN 和 LapPro 仍继续运行；不会在未确认的根分区路径静默写入大量数据。
- LapPro 使用独立的一个 pthread，不能阻塞 CAN epoll 接收线程。
- LapPro worker 每约 1 秒输出一条 INFO 状态摘要；GPS 原始逐条数据仅在 DEBUG 模式输出。
- 支持 `--replay <directory>` 和 `--replay-speed <number>`；Replay 不启动 CAN/LapPro 硬件输入，流式归并 `can0.csv`、`can1.csv`、`lappro.csv` 后复用现有 CAN 解码和 LapPro/VehicleState 汇合回调。
- Replay 同时兼容当前 logger 生成的 `can_*.csv`、`lappro_*.csv` 及完整字段格式；不读取 `vehicle_state.csv`，非法行跳过并告警。
- LapPro 通过原生 Linux Bluetooth L2CAP ATT fixed CID 4 工作，不调用 `gatttool`、`hcitool` 或 `bluetoothctl`。
- 当前默认目标地址为 `B0:A6:04:20:DF:3A`，位于 `LapProConfig` 中，可替换；默认地址类型为 LE public。
- 当前已知 ATT 参数集中定义在 `src/lappro.c`：
  - FF01 Notify value `0x002A`
  - FF01 CCCD `0x002B`
  - FF02 Write value `0x002D`
- Transport 使用 `AA 55 + TYPE + BE length + payload + LE CRC`，支持跨 Notify 重组、单次 Notify 多帧、CRC 校验和长度限制。
- 已实现四步认证：PrefixKey 派生 AuthKey、HMAC-SHA256、AES-256-CTR、RND_S/RND_C 校验和 SessionKey。
- 认证成功后读取 `product`、`active`、`bleMac`、`mtu`，再开启 `gpsDataReport`。
- TYPE 2 GPS/IMU 数据已按 27-byte GpsPacket 更新 `LapProState`。
- 当前不实现首次绑定写入流程；实机已验证这只设备认证后可直接进入正常会话。
- 程序支持 `-v`/`--verbose` 开启 DEBUG，直接运行即可开始双 CAN 和 LapPro worker：

```bash
./build/t113_terminal [-v|--verbose]
```

- 当前代码结构保持精简：
  - `src/main.c`：APP 生命周期、CAN/LapPro/logger 汇合和退出清理。
  - `src/can_receiver.c`：双 CAN epoll 接收、统计和错误处理，只生成并分发 `CanMessage`。
  - `src/can_decoder.c`：集中管理 CAN ID/signal 定义，解析并累计更新 `VehicleState`。
  - `src/gps_track.c`：GPS 经纬度到局部 XY、固定容量轨迹缓存和虚拟起点候选搜索。
  - `src/lap_timer.c`：从重复轨迹学习参考圈、虚拟起终点、圈时间和 Delta。
  - `src/accel_test.c`：静止起步、直线轨迹确认和 75m 加速计时。
  - `src/race_event.c`：双检测器、事件状态机和模式锁定。
  - `src/logger.c`：统一 `LogRecord` 固定队列、批量写入三类 CSV。
  - `src/replay.c`：Replay CSV 流式读取、按 `timestamp_us` 归并和速度控制。
  - `src/lappro.c`：LapPro worker、ATT、认证、JSON、GPS 状态更新。
  - `src/lappro_transport.c`：Transport 编解码、CRC 和分片自测。
  - `src/lappro_crypto.c`：SHA-256、HMAC-SHA256、AES-256-CTR 和固定向量自测。

### LapPro 实机验证状态

- T113 上已验证 ARMv7/hard-float 程序可通过原生 L2CAP 连接 LapPro。
- ATT MTU 协商结果为 247，FF01 Notify 订阅成功。
- Auth Step 1 重组、Step 2 发送、Step 3 解密校验、Step 4 发送均成功。
- 已收到并解密设备属性响应：`product=LapProMax`、`active=1`、`mtu=247`。
- 已开启 GPS 数据流并持续收到 TYPE 2；当前测试环境设备返回 `fix=0`、`numSV=0`，没有有效 GPS fix，程序保留该真实状态。
- CAN 与 LapPro 并行运行期间，两路 CAN 初始化和统计正常；LapPro 连接失败不会结束 APP。

## 外接 USB 存储

- 当前 T113 将 USB 设备挂载到 `/mnt/usb/sda`。
- 已通过 SSH 确认该挂载为 exFAT、`rw`，目录当前可写。
- 下一阶段 logger 的主存储目录使用 `/mnt/usb/sda`，例如 `/mnt/usb/sda/log/`。
- logger 启动时必须检查 `/mnt/usb/sda` 是否已挂载且可写；如果 USB 未挂载，不得静默把大量日志写入根分区 `/` 或 `/root`。
- 不要格式化、重新挂载或删除 `/mnt/usb/sda` 中的已有文件，除非任务明确要求。

## 构建

首次构建或 `build/` 目录为空时，先配置交叉编译：

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-t113.cmake
```

VS Code 的构建任务只执行增量构建，不会自动创建或重新配置 CMake 构建目录。

也可以直接执行：

```bash
cmake --build build -j
```

对应 VS Code 任务为 `T113: Build`，使用 GCC 问题匹配器显示编译错误。

生成的可执行文件为：

```text
build/t113_terminal
```

除非任务明确要求，不要删除或覆盖用户已有的构建产物和本地修改。

## VS Code 远程调试

项目的 `.vscode/launch.json` 和 `.vscode/tasks.json` 配置了 `T113 Remote Debug` 调试流程：

```text
T113: Build
    -> T113: Deploy
        -> T113: Start gdbserver
            -> VS Code 连接 gdbserver
```

当前配置的具体信息：

- 本地调试程序：`${workspaceFolder}/build/t113_terminal`
- 本地工作目录：`${workspaceFolder}`
- 调试器：`/usr/bin/gdb-multiarch`
- 调试方式：GDB/MI (`MIMode: gdb`)
- 远程 GDB 服务：`192.168.34.238:2345`
- 调试启动时在程序入口暂停（`stopAtEntry: true`）
- 需要在 WSL 中安装 `gdb-multiarch`，并在 VS Code 中安装 C/C++ 扩展

点击 `T113 Remote Debug` 前，必须确保：

- WSL 可以使用 `cmake`、`arm-linux-gnueabihf-gcc`、`scp`、`ssh` 和 `gdb-multiarch`；
- T113 板端已安装并可执行 `gdbserver`；
- WSL 可以通过 SSH 以 `root` 用户访问目标板，且已配置好认证；
- 目标板的 `2345` 端口可从 WSL 访问。

## T113 远程访问

- 通过 SSH 访问 T113 开发板。
- 当前已知 IP 为 `192.168.34.238`，但该地址可能变化。
- 当前 VS Code 配置使用 `root@192.168.34.238`，不要把这个地址当作永久地址。
- 如果 IP 发生变化，需要同步修改 `.vscode/launch.json` 中的 `miDebuggerServerAddress`，以及 `.vscode/tasks.json` 中 `T113: Deploy` 和 `T113: Start gdbserver` 的 SSH/SCP 地址。
- 不要把 SSH 密钥、密码或令牌提交到仓库。

手动检查连接时，可以使用环境变量保存当前地址：

```bash
export T113_HOST=192.168.34.238
ssh root@"$T113_HOST"
```

如果已在 `~/.ssh/config` 中配置主机别名，应优先使用别名。当前 VS Code 任务尚未使用 `T113_HOST` 环境变量，因此仅设置该变量不会自动更新任务中的目标地址。

## 部署与 gdbserver

当前 VS Code 部署任务执行：

```bash
scp build/t113_terminal root@192.168.34.238:/mnt/UDISK/t113_terminal
```

程序部署到板端后，启动任务会执行以下操作：

```bash
ssh root@192.168.34.238 \
  'chmod +x /mnt/UDISK/t113_terminal; \
   pkill gdbserver 2>/dev/null || true; \
   nohup gdbserver :2345 /mnt/UDISK/t113_terminal \
     >/tmp/gdbserver.log 2>&1 </dev/null &'
```

该操作会结束板端已有的 `gdbserver` 进程，并在 `2345` 端口后台启动新的服务。调试异常时，可在板端查看：

```bash
ssh root@"$T113_HOST" 'cat /tmp/gdbserver.log'
```

除非明确要求，不要删除远程文件、重启设备或修改系统服务。

## 修改约定

- 保持 CMake 配置与交叉编译工具链可用。
- 新增功能时优先修改 `src/`，不要把生成文件提交到源码目录。
- 修改后应至少检查 CMake 配置和本地可用的构建步骤；如果交叉编译器或目标板不可用，应明确说明未完成的验证项。
- 不要提交 SSH 私钥、密码、令牌或包含敏感信息的本地配置。

## Git 提交规范

- 每次功能、修复、重构或文档变更完成并验证后，按功能创建 Git 提交。
- 单个提交只包含一个清晰主题，不要把无关功能、临时文件或构建产物混入同一提交。
- 提交前检查 `git diff --check`，并运行与改动相关的构建或测试。
- 提交信息使用简洁的动作描述，并优先采用以下前缀：
  - `feat:`：新增功能
  - `fix:`：问题修复
  - `refactor:`：不改变外部行为的重构
  - `docs:`：文档变更
  - `test:`：测试变更
- 用户明确保留的未跟踪文件或与当前任务无关的修改不得自动加入提交。
