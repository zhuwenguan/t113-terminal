# T113 上车前工程化加固

更新日期：2026-09-23。

本次加固沿用现有的 Buildroot/SysV 架构。核心采集仍由 `race_core` 负责，视频和 HTTP 仍为独立进程；没有把 CAN、LapPro、HUD、编码器或 MP4 writer 合并重写。

## 当前架构审查结论

启动入口是 `deploy/t113/init.d/S75race`，它调用 `scripts/start_race.sh`，再由 `scripts/race_supervisor.sh` 管理 `race_core`、`race_video` 和 `webd`。`race_core` 内部拥有双 SocketCAN、CAN 解码、LapPro、VehicleState、HMI、赛事算法和 logger；logger 线程在 USB 存储可用后建立 session。`race_core_status` 共享内存提供 logger、CAN、HMI、LapPro、生命周期和心跳状态，视频/HTTP 只读这些状态，不反向控制采集。

原有设计已经具备 CAN 单路隔离与重连、logger 存储前置检查、MP4 分段、异步 writer、信号退出和状态共享。主要缺口是：启动脚本没有 status/部署回滚入口，supervisor 日志不持久且没有大小限制，没有周期性 health 文件，没有 watchdog 兜底，视频新分段没有剩余空间门禁。

仓库中没有板端原厂 `S39wifi`、`S40network` 或 `S50adb_start` 的实现文件；它们只在项目背景文档中被记录为镜像侧服务。因此本次没有复制或替代 WiFi、网络、ADB 服务，仍让 Buildroot 原有顺序先完成设备/网络，再由本项目的 S75/S95 入口启动终端。

## 修改清单

| 文件 | 修改 | 目的 |
| --- | --- | --- |
| `deploy/t113/init.d/S75race` | 增加 `status`，显示 core 状态和各进程 PID | 无 SSH 时确认运行状态 |
| `deploy/t113/init.d/S95t113-terminal` | 增加标准 P0 SysV 服务名 | 适配需要 S95 启动顺序的镜像 |
| `scripts/start_race.sh` | 默认使用 USB 日志目录；未挂载时退回 `/tmp/race`；加载 `terminal.conf` | 上电自动启动且避免把大量日志写入根分区 |
| `scripts/race_supervisor.sh` | 启动 health logger/watchdog；异常重启记录；应用日志就地限长；停止时清理子进程 | 自动恢复与低频故障诊断 |
| `scripts/health_log.sh` | 每秒生成 `health.csv`，包含 uptime、内存、磁盘、core、CAN 计数、进程和重启次数 | 不依赖 SSH 获取运行状态 |
| `tools/t113_watchdog.c` | 使用 `/dev/watchdog`，只在 core 心跳失效且 core 进程仍存在时停止喂狗 | 处理进程/系统 hang；core crash 留给 supervisor 恢复 |
| `libs/ipc/include/race/core_status.h`、`main.c`、`race_core_status_dump.c` | 状态 ABI 升级到 v2，发布 CAN rx/error/bus-off 累计值 | health log 能区分“接口活着”和“有数据/有故障” |
| `apps/terminal-core/video/*` | 新分段前检查 `statvfs`；最低空间由 `RACE_MIN_FREE_BYTES` 配置，默认 128 MiB | 存储接近耗尽时禁止新录像而不影响核心采集 |
| `configs/t113/terminal.conf` | 集中放置日志、watchdog、health、最小空间参数 | 运行时可调整，不再散落在启动脚本 |
| `scripts/install.sh`、`rollback.sh`、`status.sh`、`restart.sh`、`collect_logs.sh` | 统一安装、回滚、控制和诊断入口 | 减少现场手工部署步骤 |

## 启动流程

```text
Boot
  ↓
Buildroot SysV: S39/S40 等基础服务、设备和网络
  ↓
S75race 或 S95t113-terminal
  ↓
start_race.sh：加载配置、确认 USB 日志挂载、创建 /run/race
  ↓
race_supervisor.sh + health_log.sh
  ↓
race_core：CAN / logger / LapPro / HMI / VehicleState
  ↓ core status = RUNNING + CAN mask = 0x3 + logger ready
race_video、webd
```

视频和 HTTP 的启动仍由 supervisor 延迟到 core `RUNNING`，因此摄像头或 HTTP 初始化失败不会阻塞 CAN/logger。LapPro 仍由 core 内部独立 worker 管理，连接失败只会保留 unavailable 状态。

## 故障恢复

| 故障 | 当前处理 |
| --- | --- |
| `race_core` 异常退出 | supervisor 记录 `supervisor.log`，按 1、2、4…30 秒退避后重启；正常 stop 设置 supervisor stop flag，不会自重启 |
| core 线程/进程 hang | watchdog 读取 `/race_core_status` 心跳；core PID 仍存在且超过 timeout 后停止喂狗，由硬件 watchdog 复位整机 |
| CAN0/CAN1 bus-off、断开 | 现有 receiver 按 bus 隔离、重连并发布 active mask；另一条总线继续工作；累计 rx/error/bus-off 写入 health |
| Camera 断开/打开失败 | `race_video` 进程与 core 分离，退出后 supervisor 重启；CAN/logger 不受影响 |
| LapPro/Bluetooth 不可用 | core 保持运行，LapPro worker 可重连；CAN/logger/video 不依赖有效 fix 才能存活 |
| HTTP/WiFi/client 异常 | `webd` 独立进程，客户端上限和 IPC 缓冲沿用现有实现；HTTP 退出不影响 core/video |
| 存储低于阈值 | logger 的 USB mount/read-only 检查继续生效；视频不开始新的分段；VehicleState/CAN 仍运行 |
| 正常 SIGTERM/SIGINT | 现有顺序停止接收、snapshotter、LapPro、视频、logger 并 `sync()`；MP4 finalize 和 logger flush 在各自 stop 中完成 |
| 暴力断电 | 最后一个 MP4 允许损坏；logger 启动时清理 CSV 不完整尾行，后续 session 使用不覆盖目录；MP4 原有 recover 工具保留 |

watchdog 只有目标存在 `/dev/watchdog` 字符设备且状态共享内存可读时才会启动。内核启用 `nowayout` 时，正常 stop 的关闭语义由目标内核决定，必须在板端验证，不能由主机测试假定。

## 部署

在开发机完成 ARM 构建和安装树生成：

```bash
cmake --preset arm-release
cmake --build --preset arm-release --parallel
DESTDIR="$PWD/out/t113-release" cmake --install out/arm-release --prefix /opt/t113
tar -C out/t113-release -czf out/t113-release.tar.gz opt/t113
```

将压缩包复制到板端后，现场只需要执行一次：

```bash
scp out/t113-release.tar.gz root@BOARD:/tmp/
ssh root@BOARD 'rm -rf /tmp/t113-release && mkdir -p /tmp/t113-release && tar -C /tmp/t113-release -xzf /tmp/t113-release.tar.gz && /tmp/t113-release/opt/t113/bin/install.sh /tmp/t113-release/opt/t113'
```

之后不再需要人工启动：

```bash
/opt/t113/etc/init.d/S95t113-terminal start
/opt/t113/bin/status.sh
/opt/t113/bin/restart.sh
/opt/t113/bin/collect_logs.sh
/opt/t113/bin/rollback.sh
```

`install.sh` 会先停止旧服务并保存 `/opt/t113.previous`，再复制新版本、设置脚本权限、启动并执行一次 status。若新版本不能工作，`rollback.sh` 停止服务、交换回 previous 目录并启动旧版本。安装包不包含密钥、密码或 SSH 配置。

## 测试报告

| 测试 | 执行 | 结果 |
| --- | --- | --- |
| Host build | `cmake --preset host-debug && cmake --build --preset host-debug --parallel 4` | PASS |
| Host tests | `ctest --preset host-debug` | 23/23 PASS |
| Shell syntax | `sh -n scripts/*.sh deploy/t113/init.d/*` | PASS |
| Install tree | `cmake --install out/host-debug --prefix out/review-install` | PASS；包含 watchdog、health、控制脚本、两个 init 名称和配置 |
| ARM core/video/watchdog build | `cmake --preset arm-release && cmake --build --preset arm-release --target race_core race_video t113_watchdog --parallel 4` | PASS |
| Cold boot | 板端断电后上电，检查 `status.sh`、CAN、logger、camera、video、HTTP、LapPro | 未在本环境执行 |
| Process crash | 板端 `kill -9` core，观察 supervisor.log 和 PID | 未在本环境执行 |
| Camera/CAN/LapPro/network/storage fault | 板端注入故障，确认核心继续 | 未在本环境执行 |
| Hard power cut | 录像和 CAN 写入期间直接断电，多次上电 | 未在本环境执行 |

## 未完成项

| 问题 | 原因 | 当前状态与建议 |
| --- | --- | --- |
| 实机 watchdog reset 行为 | 主机没有 `/dev/watchdog`，内核 `nowayout` 未知 | 工具已交叉编译；上车前用真实板端做一次“core hang → reset”和一次正常 stop 验证 |
| camera 自动重连的帧级语义 | video 现有 pipeline 重启由进程级 supervisor 完成 | 可接受为首阶段隔离；若驱动只返回 timeout 而不退出，需要板端确认 pipeline fault 是否触发退出 |
| logger write failure 后持续恢复 | logger supervisor 会重建 session，但 USB 真正恢复的时序依赖挂载系统 | 板端拔插 USB 后验证；不要把根分区当作隐式 fallback |
| WiFi AP/DHCP | 由原厂 Buildroot 服务负责，本仓库不重复实现 | 仍需 cold boot 验证 S39/S40 与 S95 顺序 |
| 4G telemetry、复杂 HUD、云端 | 明确不在本次范围 | 未修改 |
