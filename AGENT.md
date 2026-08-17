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
- 每帧统一转换为包含 `timestamp_us`、`bus`、`can_id`、`dlc` 和 `data[8]` 的 `CanMessage`。
- 时间戳使用 `CLOCK_MONOTONIC`，单位为微秒。
- 当前阶段只打印原始 CAN 帧，不加入日志、协议解析、蓝牙、数据库或 GUI。
- 第二阶段增加每路 `rx_frames`、错误帧、非法帧、读取错误、epoll 错误和 bus-off 统计。
- SocketCAN 已启用 error frame 过滤；检测到 `CAN_ERR_BUSOFF` 时标记接口失败并停止接收，避免静默运行。
- 程序退出时打印两路 CAN 的统计汇总。
- 第三阶段增加一个 logger 线程和固定容量内存队列；接收线程只入队，不直接执行文件写入。
- logger 每最多 32 帧或 1 秒批量写入一次 CSV，不对每帧执行 `fsync`。
- 每次运行生成独立的 `/mnt/usb/sda/log/can_*.csv` 文件，并统计入队、写入、丢弃和写错误数量。
- logger 不可用时只输出告警，CAN 和 LapPro 仍继续运行；不会在未确认的根分区路径静默写入大量数据。
- LapPro 使用独立的一个 pthread，不能阻塞 CAN epoll 接收线程。
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
- 程序无命令行参数，直接运行即可开始双 CAN 和 LapPro worker：

```bash
./build/t113_terminal
```

- 当前代码结构保持精简：
  - `src/main.c`：APP 生命周期、CAN/LapPro/logger 汇合和退出清理。
  - `src/can_receiver.c`：双 CAN epoll 接收、统计和错误处理。
  - `src/logger.c`：固定队列、批量 CSV 写入。
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
