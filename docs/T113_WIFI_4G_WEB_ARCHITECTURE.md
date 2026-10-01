# T113 数据终端 Wi‑Fi AP + 4G 遥测 Web 架构设计

## 1. 目标

T113 数据终端需要同时支持两种访问方式：

1. **本地 Wi‑Fi AP**
   - T113 自身作为 Wi‑Fi 热点和局域网主机。
   - 手机、平板、电脑直接连接 T113。
   - 浏览器访问 T113 本地 HTTP Server。
   - 查看实时 VehicleState、圈速、BMS、系统状态和车载画面。
   - 本地链路不依赖公网。

2. **4G 远程遥测**
   - T113 通过 EC20 等 4G 模块主动连接公网服务器。
   - 远程用户访问公网 Web Server。
   - 查看与本地界面尽可能一致的实时遥测界面。
   - 远程数据来自 T113 上传的 VehicleState。
   - 第一阶段远程视频可选，本地视频必须保留。

3. **前端共用**
   - 本地 T113 Web Server 与公网服务器使用同一套 HTML/CSS/JavaScript 前端。
   - 开发者只维护一个前端工程。
   - 构建后同时发布到：
     - T113 本地静态资源目录；
     - 公网服务器静态资源目录。

核心原则：

> `terminal-core` 只负责车辆数据生产，不感知 Wi‑Fi、本地 HTTP、公网服务器和浏览器。

---

# 2. 总体架构

```text
                                  ┌──────────────────────┐
                                  │   Remote Browser     │
                                  │ 手机 / 平板 / PC      │
                                  └──────────┬───────────┘
                                             │ HTTPS
                                             ▼
                                  ┌──────────────────────┐
                                  │   Public Web Server  │
                                  │ 静态网页 + WS/API     │
                                  └──────────┬───────────┘
                                             │
                                      Cloud Telemetry
                                             ▲
                                             │ MQTT/TLS
                                             │
                                      ┌──────┴──────┐
                                      │ telemetryd  │
                                      └──────▲──────┘
                                             │ IPC
                                             │
┌────────────────────────────────────────────┼─────────────────────────────────┐
│                                 T113 TERMINAL                               │
│                                                                            │
│                    ┌────────────────────────────┐                          │
│ CAN / LapPro / GPS │                            │                          │
│ SOC / Lap Timer ──▶│       terminal-core        │                          │
│                    │                            │                          │
│                    │  VehicleState Producer     │                          │
│                    └─────────────┬──────────────┘                          │
│                                  │                                         │
│                        Local IPC / Shared State                            │
│                                  │                                         │
│               ┌──────────────────┴──────────────────┐                     │
│               │                                     │                     │
│               ▼                                     ▼                     │
│        ┌───────────────┐                      ┌───────────────┐            │
│        │     webd      │                      │  telemetryd   │            │
│        │ Local HTTP/WS │                      │ 4G Uplink     │            │
│        └───────┬───────┘                      └───────────────┘            │
│                │                                                           │
│      VehicleState + Video                                                  │
│                │                                                           │
│        Wi‑Fi AP / LAN                                                       │
└────────────────┼───────────────────────────────────────────────────────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │    Local Browser     │
      │ 手机 / 平板 / PC      │
      └──────────────────────┘
```

---

# 3. 不推荐的结构

不要设计成：

```text
terminal-core
    ├── HTTP Local Client/Server
    ├── Cloud HTTP Client
    ├── WebSocket
    ├── MQTT
    ├── Video Upload
    └── Wi‑Fi management
```

这种结构会让主程序继续膨胀，并把网络异常带入车辆核心程序。

例如公网服务器阻塞、4G 掉线、DNS 卡住、浏览器连接过慢，都不应该影响：

- CAN 接收；
- LapPro；
- 圈速计算；
- VehicleState 更新；
- Logger；
- Camera；
- 本地录像。

因此网络必须放在独立 service/process 中。

---

# 4. 推荐仓库目录

推荐将整个项目逐渐整理为：

```text
t113-terminal/
│
├── apps/
│   │
│   ├── terminal-core/
│   │   ├── main.c
│   │   ├── vehicle_state_manager.c
│   │   ├── vehicle_state_manager.h
│   │   └── ...
│   │
│   ├── webd/
│   │   ├── main.c
│   │   ├── http_server.c
│   │   ├── websocket_server.c
│   │   ├── api_state.c
│   │   ├── api_system.c
│   │   ├── api_logs.c
│   │   ├── video_stream.c
│   │   └── static_files.c
│   │
│   ├── telemetryd/
│   │   ├── main.c
│   │   ├── telemetry_client.c
│   │   ├── mqtt_client.c
│   │   ├── telemetry_encoder.c
│   │   ├── reconnect.c
│   │   ├── event_queue.c
│   │   └── network_status.c
│   │
│   ├── loggerd/                 # 后续可逐渐拆出
│   │   └── ...
│   │
│   └── video-uplinkd/           # 远程视频需要时再增加
│       ├── main.c
│       ├── frame_reader.c
│       └── uploader.c
│
├── libs/
│   │
│   ├── vehicle-state/
│   │   ├── vehicle_state.h
│   │   ├── vehicle_state.c
│   │   └── vehicle_state_version.h
│   │
│   ├── ipc/
│   │   ├── state_shm.c
│   │   ├── state_shm.h
│   │   ├── event_socket.c
│   │   └── event_socket.h
│   │
│   ├── protocol/
│   │   ├── telemetry_protocol.h
│   │   ├── telemetry_json.c
│   │   ├── telemetry_binary.c
│   │   └── protocol_version.h
│   │
│   ├── video-ipc/
│   │   ├── latest_frame.c
│   │   └── latest_frame.h
│   │
│   └── common/
│       ├── log.c
│       ├── time.c
│       ├── config.c
│       └── ...
│
├── web/
│   └── dashboard/
│       ├── package.json
│       ├── src/
│       │   ├── main.js
│       │   ├── app.js
│       │   │
│       │   ├── api/
│       │   │   ├── state.js
│       │   │   ├── system.js
│       │   │   └── logs.js
│       │   │
│       │   ├── transport/
│       │   │   ├── telemetry_ws.js
│       │   │   ├── local_video.js
│       │   │   └── capabilities.js
│       │   │
│       │   ├── store/
│       │   │   ├── vehicle_state.js
│       │   │   └── connection_state.js
│       │   │
│       │   ├── pages/
│       │   │   ├── live/
│       │   │   ├── battery/
│       │   │   ├── laps/
│       │   │   ├── system/
│       │   │   └── logs/
│       │   │
│       │   └── components/
│       │       ├── speed/
│       │       ├── battery/
│       │       ├── lap_timer/
│       │       ├── video/
│       │       └── status/
│       │
│       └── dist/
│
├── cloud/
│   │
│   ├── telemetry-gateway/
│   │   ├── src/
│   │   └── ...
│   │
│   ├── web-backend/
│   │   ├── src/
│   │   └── ...
│   │
│   └── media/
│       └── ...
│
├── configs/
│   ├── t113/
│   │   ├── webd.conf
│   │   ├── telemetryd.conf
│   │   ├── hostapd.conf
│   │   └── udhcpd.conf
│   │
│   └── cloud/
│       └── ...
│
├── deploy/
│   ├── t113/
│   │   ├── install.sh
│   │   ├── init/
│   │   └── rootfs/
│   │
│   └── cloud/
│       ├── nginx/
│       └── ...
│
├── scripts/
│   ├── build-all.sh
│   ├── build-web.sh
│   ├── deploy-t113.sh
│   └── deploy-cloud.sh
│
├── tests/
│   ├── ipc/
│   ├── webd/
│   ├── telemetry/
│   └── protocol/
│
└── docs/
    ├── ARCHITECTURE.md
    ├── WEB_API.md
    ├── TELEMETRY_PROTOCOL.md
    └── DEPLOYMENT.md
```

---

# 5. `terminal-core` 的职责

`terminal-core` 继续作为赛车数据核心。

它负责：

```text
CAN
LapPro
GPS
SOC
Lap Timer
Accel Timer
Vehicle Logic
Fault State
```

最终形成统一的：

```c
VehicleState
```

例如：

```c
typedef struct {
    uint64_t monotonic_us;
    uint64_t utc_ms;

    float speed_kph;

    int32_t motor_rpm;
    float torque_nm;

    float hv_voltage_v;
    float hv_current_a;
    float hv_power_kw;
    float hv_soc_percent;

    float lv_voltage_v;

    float throttle_percent;
    float brake_percent;

    float gps_lat;
    float gps_lon;

    int32_t lap_number;
    float lap_time_s;
    float delta_s;

    uint32_t vehicle_flags;
    uint32_t fault_flags;
} VehicleState;
```

关键原则：

```text
terminal-core 不发送 HTTP
terminal-core 不发送 MQTT
terminal-core 不关心公网地址
terminal-core 不关心浏览器是否在线
terminal-core 不关心 Wi‑Fi 客户端数量
```

它只做：

```text
Update VehicleState
        ↓
Publish Local State
```

---

# 6. VehicleState 的 IPC

## 6.1 高频实时状态

推荐：

```text
POSIX Shared Memory
```

用途：

```text
terminal-core
      │
      ▼
VehicleState SHM
      │
 ┌────┴─────┐
 ▼          ▼
webd    telemetryd
```

例如：

```text
/dev/shm/t113_vehicle_state
```

逻辑上只保存：

```text
Latest VehicleState
```

消费者不应该要求 core 保留历史状态。

建议加入：

```c
typedef struct {
    uint32_t magic;
    uint32_t version;

    uint64_t sequence;

    VehicleState state;
} SharedVehicleState;
```

读取方式：

```text
读 sequence A
读 VehicleState
读 sequence B

A == B
    → 数据有效

A != B
    → 重读
```

或者使用双缓冲/seqlock。

这样可以做到：

- 不需要 mutex 跨进程阻塞；
- webd 慢不会拖住 core；
- telemetryd 崩溃不会影响 core。

---

# 7. 事件不能只靠 VehicleState

有些信息是瞬时事件，例如：

```text
LapCompleted
Accel50mCompleted
Accel75mCompleted
BMSFaultRaised
HVOn
HVOff
RecordingStarted
RecordingStopped
Marker
```

如果只读取 Latest VehicleState，消费者可能错过这些事件。

所以建议额外提供：

```text
Unix Domain Socket
```

或者轻量事件队列：

```text
/run/t113/events.sock
```

形成：

```text
terminal-core
    │
    ├── Shared Memory
    │      └── 当前状态
    │
    └── Event Socket
           └── 离散事件
```

这样：

```text
VehicleState = 状态
EventBus     = 事件
```

职责非常清楚。

---

# 8. `webd` 的职责

`webd` 是本地 Wi‑Fi Web Server。

它只服务：

```text
192.168.10.1
```

建议职责：

```text
Static HTML/CSS/JS
REST API
WebSocket telemetry
Local video stream
Log download
System status
```

接口可以统一成：

```text
GET  /api/state
GET  /api/system
GET  /api/capabilities
GET  /api/logs
GET  /api/config

WS   /ws/telemetry

GET  /video/live.mjpg
```

浏览器访问：

```text
http://192.168.10.1/
```

---

# 9. `telemetryd` 的职责

`telemetryd` 不提供网页。

它是：

> T113 → 公网服务器的主动连接客户端。

结构：

```text
Shared VehicleState
       │
       ▼
 telemetryd
       │
       ├── fast telemetry
       ├── slow telemetry
       ├── events
       └── system/network state
       │
       ▼
   MQTT/TLS
       │
       ▼
 Public Server
```

它负责：

- 读取 VehicleState；
- 订阅 terminal-core 事件；
- 数据降频；
- 数据编码；
- MQTT/TLS；
- 自动重连；
- 网络断线处理；
- 重要事件持久化补传；
- 4G 网络质量统计。

而不负责：

- CAN；
- 圈速计算；
- 摄像头；
- 本地 HTTP；
- HTML。

---

# 10. telemetry 数据建议分频

不要所有数据统一 50 Hz 上传。

推荐：

## Fast

```text
10~20 Hz
```

例如：

```text
speed
rpm
torque
throttle
brake
power
lap_time
delta
```

## Slow

```text
1~2 Hz
```

例如：

```text
SOC
HV Voltage
temperatures
GPS
CPU
RAM
disk
4G signal
CAN status
recording status
```

## Event

立即发送：

```text
lap completed
fault raised
fault cleared
accel completed
recording start/stop
HV on/off
```

---

# 11. 公网服务器的职责

公网服务器不是第二个 `terminal-core`。

它只负责：

```text
T113 telemetry input
        ↓
Current VehicleState Cache
        ↓
WebSocket
        ↓
Remote Browser
```

结构：

```text
MQTT Broker
     │
     ▼
Telemetry Gateway
     │
     ├── Current State Cache
     ├── Event Store
     └── History Database
     │
     ▼
Web Backend
     │
     ├── /api/state
     ├── /api/system
     ├── /api/capabilities
     └── /ws/telemetry
     │
     ▼
Browser
```

---

# 12. 本地和公网 HTTP API 应尽量一致

这是前端共用的关键。

本地：

```text
http://192.168.10.1/api/state
ws://192.168.10.1/ws/telemetry
```

公网：

```text
https://telemetry.example.com/api/state
wss://telemetry.example.com/ws/telemetry
```

但 JavaScript 不写死域名。

永远使用：

```javascript
fetch("/api/state");
```

以及：

```javascript
const proto =
    location.protocol === "https:" ? "wss:" : "ws:";

const ws = new WebSocket(
    `${proto}//${location.host}/ws/telemetry`
);
```

于是同一套构建结果：

```text
dist/
```

放到 T113：

```text
/opt/t113/web/
```

可以工作。

放到公网：

```text
/var/www/t113-dashboard/
```

也可以工作。

---

# 13. 前端不要直接判断“我现在在本地还是公网”

不要写：

```javascript
if (location.hostname === "192.168.10.1") {
    ...
}
```

推荐提供：

```text
GET /api/capabilities
```

本地服务器返回：

```json
{
  "mode": "local",
  "telemetry": true,
  "video": true,
  "logs": true,
  "config": true,
  "remote_video": false
}
```

公网服务器返回：

```json
{
  "mode": "cloud",
  "telemetry": true,
  "video": false,
  "logs": false,
  "config": false,
  "remote_video": false
}
```

前端根据 capability 决定：

```text
显示什么
隐藏什么
禁用什么
```

这样以后增加远程视频时只需要改：

```json
"remote_video": true
```

前端架构不需要重写。

---

# 14. Web 前端建议的数据层

页面组件不要直接操作 WebSocket。

推荐：

```text
WebSocket
    │
    ▼
TelemetryTransport
    │
    ▼
VehicleStateStore
    │
    ├── Speed Widget
    ├── Battery Widget
    ├── Lap Widget
    ├── HUD
    └── Graph
```

例如：

```text
src/
    transport/
        telemetry_ws.js

    store/
        vehicle_state.js

    components/
        speed/
        battery/
        lap/
```

这样：

```text
本地 WebSocket
```

和：

```text
公网 WebSocket
```

对 UI 来说完全相同。

---

# 15. Web 前端发布流程

开发者只维护：

```text
web/dashboard/
```

开发：

```text
npm run dev
```

正式构建：

```text
npm run build
```

得到：

```text
web/dashboard/dist/
```

然后：

```text
                dist/
                  │
        ┌─────────┴─────────┐
        │                   │
        ▼                   ▼
T113 local server      Public server
/opt/t113/web/         /var/www/dashboard/
```

建议制作：

```text
scripts/deploy-web-t113.sh
scripts/deploy-web-cloud.sh
```

甚至：

```text
scripts/deploy-web-all.sh
```

统一执行：

```text
build
  ↓
deploy T113
  ↓
deploy cloud
```

---

# 16. 不建议在 T113 上跑 Node.js 前端服务

T113 运行的应该只是已经构建好的：

```text
HTML
CSS
JavaScript
PNG/SVG/font
```

例如：

```text
/opt/t113/web/
    index.html
    assets/
        app.js
        app.css
```

`webd` 只负责静态文件发送。

真正的 JavaScript 运行在：

```text
手机
平板
PC浏览器
```

而不是 T113。

---

# 17. 视频链路应该与 VehicleState 完全分离

本地视频：

```text
Camera
   │
   ▼
Video Pipeline
   │
   ├── Recorder
   │
   └── Latest JPEG Frame
             │
             ▼
            webd
             │
             ▼
        Local Browser
```

不要：

```text
terminal-core
      │
      └── VehicleState IPC
             └── Video
```

VehicleState 和 Video 是两种完全不同的数据：

```text
VehicleState
    小
    高频
    最新值

Video
    大
    高频
    只要最新帧
```

应该分开。

---

# 18. 视频 IPC

推荐为视频定义：

```text
LatestFrame
```

例如：

```c
typedef struct {
    uint64_t sequence;
    uint64_t pts_us;

    uint32_t width;
    uint32_t height;

    uint32_t jpeg_size;

    uint8_t jpeg_data[MAX_JPEG_SIZE];
} LatestJpegFrame;
```

Video Pipeline 不维护 Web Client 队列。

只做：

```text
New JPEG
   ↓
Replace LatestFrame
```

`webd`：

```text
读取最新 sequence
        ↓
send()
        ↓
客户端慢？
        ↓
直接跳过旧帧
```

因此：

```text
Web客户端卡顿
```

不会导致：

```text
Camera卡顿
Recorder卡顿
```

---

# 19. 本地视频和远程视频不要共用上传逻辑

第一阶段：

```text
video
  │
  └── webd
       └── local browser
```

以后需要 4G 视频：

```text
video
  │
  ├── webd
  │    └── local
  │
  └── video-uplinkd
       └── 4G
```

也就是说：

```text
video-uplinkd
```

是独立 service。

不要把远程视频代码塞进：

```text
telemetryd
```

因为二者特征差异非常大：

```text
Telemetry
    KB/s
    可以保存重要事件
    可以补传

Video
    MB/s
    不允许积压
    永远追最新帧
```

---

# 20. 进程关系

建议正式系统逐渐形成：

```text
terminal-core
     │
     ├────────── vehicle state SHM ─────────┐
     │                                      │
     └────────── event socket ──────────────┤
                                            │
                                  ┌─────────┴─────────┐
                                  │                   │
                                webd             telemetryd
                                  │                   │
                              local WiFi             4G
```

视频：

```text
video-service
     │
     ├── recorder
     │
     └── latest frame SHM
               │
               ▼
              webd
```

后期：

```text
latest frame SHM
       │
       ├── webd
       └── video-uplinkd
```

---

# 21. 进程故障隔离要求

系统必须满足：

```text
kill webd
```

结果：

```text
网页不可访问
```

但：

```text
CAN          OK
Lap          OK
Logger       OK
Recorder     OK
Telemetry    OK
```

---

```text
kill telemetryd
```

结果：

```text
远程遥测离线
```

但：

```text
Local Web    OK
CAN          OK
Recorder     OK
```

---

```text
拔掉 EC20
```

结果：

```text
4G Remote    OFFLINE
```

但其他功能全部正常。

---

```text
Wi-Fi AP 崩溃
```

也不能影响：

```text
4G telemetry
```

这就是拆 service 的核心价值。

---

# 22. 配置文件建议

不要把 IP、域名、采样频率写死在代码里。

例如：

```text
/etc/t113/webd.conf
```

```ini
listen_address=0.0.0.0
listen_port=80
web_root=/opt/t113/web

telemetry_rate_hz=20
video_enabled=true
```

---

```text
/etc/t113/telemetryd.conf
```

```ini
enabled=true

server=mqtt.example.com
port=8883

vehicle_id=car01

fast_rate_hz=20
slow_rate_hz=1

tls=true
```

---

# 23. VehicleState 版本管理

这是后面非常容易踩坑的地方。

例如最初：

```c
VehicleState v1
```

后面增加：

```text
motor_temp
inverter_temp
cell_min_voltage
cell_max_voltage
```

如果直接修改 struct，而旧的：

```text
webd
telemetryd
replay
tests
```

还没有同步升级，很容易产生 IPC 数据错位。

所以必须有：

```c
#define VEHICLE_STATE_VERSION 3
```

共享内存头：

```c
uint32_t magic;
uint32_t version;
uint32_t struct_size;
```

程序启动时验证：

```text
magic
version
size
```

不匹配立即拒绝读取。

---

# 24. Cloud 协议不要直接发送内存 struct

IPC 可以使用 C struct。

但：

```text
T113 → Cloud
```

不要直接把：

```c
sizeof(VehicleState)
```

的数据原样发送。

原因：

- 字节序；
- struct padding；
- ARM/x86 差异；
- 协议升级；
- 字段兼容；
- 调试困难。

公网遥测需要明确协议层：

```text
VehicleState
     │
     ▼
Telemetry Encoder
     │
     ├── JSON     # 初期
     │
     └── Binary   # 后期
```

因此：

```text
IPC Struct
```

和：

```text
Network Protocol
```

必须是两个概念。

---

# 25. 第一阶段可以先用 JSON

例如：

```json
{
  "protocol": 1,
  "vehicle": "car01",
  "timestamp": 1788253200123,
  "speed": 82.4,
  "rpm": 8431,
  "torque": 118.2,
  "hv_voltage": 383.1,
  "hv_current": 126.5,
  "soc": 74.2,
  "lap": 4,
  "lap_time": 28.721,
  "delta": -0.182
}
```

优点：

```text
人能直接看
curl 能调
浏览器容易验证
服务端容易处理
```

等系统成熟后，如果认为流量值得优化，再增加：

```text
binary protocol
CBOR
protobuf
```

没必要第一版就复杂化。

---

# 26. 本地网页和云端网页的数据路径

## Local

```text
terminal-core
      ↓
VehicleState SHM
      ↓
webd
      ↓
WebSocket
      ↓
Browser
```

延迟最低。

不要设计成：

```text
terminal-core
 ↓
4G
 ↓
cloud
 ↓
Wi-Fi本地手机
```

本地永远走本地链路。

---

## Remote

```text
terminal-core
      ↓
VehicleState SHM
      ↓
telemetryd
      ↓
MQTT/TLS
      ↓
Cloud
      ↓
Cloud WebSocket
      ↓
Remote Browser
```

---

# 27. 同一套 HTML 的核心约定

最重要的是：

> 前端只知道“当前服务器”，不知道数据到底来自赛车本机还是云端。

调用：

```text
/api/state
/ws/telemetry
/api/system
/api/capabilities
```

本地由：

```text
webd
```

实现。

公网由：

```text
web-backend
```

实现。

这样前端完全不需要复制。

---

# 28. API 语义必须保持一致

例如本地：

```json
{
  "speed_kph": 82.4,
  "motor_rpm": 8431,
  "hv_voltage_v": 383.1,
  "hv_current_a": 126.5,
  "hv_soc_percent": 74.2
}
```

公网：

```json
{
  "speed_kph": 82.4,
  "motor_rpm": 8431,
  "hv_voltage_v": 383.1,
  "hv_current_a": 126.5,
  "hv_soc_percent": 74.2
}
```

不要让本地叫：

```text
rpm
```

云端叫：

```text
motorSpeed
```

否则前端很快变成大量：

```javascript
if (local)
else
```

---

# 29. 推荐的 Web 页面结构

```text
/
    Dashboard

/live
    Live HUD
    Video
    Speed
    RPM
    Power
    SOC
    Lap
    Delta

/battery
    HV
    Current
    Power
    SOC
    Temperatures
    BMS Fault

/laps
    Current Lap
    Previous Laps
    Best Lap
    Delta

/system
    CPU
    RAM
    Disk
    CAN
    LapPro
    Camera
    Recording
    Wi-Fi
    4G

/logs
    Local only initially
```

本地：

```text
/logs
```

正常显示。

公网 capability：

```text
logs=false
```

则隐藏。

---

# 30. 服务启动建议

最终 Buildroot init 可以逐渐变成：

```text
S30network
S31wifi-ap
S40terminal-core
S41video
S42logger
S50webd
S51telemetryd
```

依赖关系不要写成：

```text
webd failed
    ↓
terminal-core 不启动
```

正确思想是：

```text
terminal-core 是核心服务

webd / telemetryd
属于附属服务
```

它们失败后应单独重启。

---

# 31. 推荐开发阶段

## Phase 1：统一 VehicleState IPC

首先实现：

```text
terminal-core
       ↓
VehicleState SHM
       ↓
test reader
```

验证：

- 20/50/100 Hz 更新稳定；
- 无 torn read；
- core 不被 reader 阻塞。

---

## Phase 2：Local Web

实现：

```text
webd
```

支持：

```text
/api/state
/ws/telemetry
```

浏览器连接：

```text
192.168.10.1
```

实时显示 VehicleState。

---

## Phase 3：统一前端

建立：

```text
web/dashboard/
```

彻底替代：

```text
T113专用网页
云端专用网页
```

只保留：

```text
一套 frontend
```

---

## Phase 4：telemetryd

```text
VehicleState SHM
      ↓
telemetryd
      ↓
MQTT
      ↓
Cloud
```

---

## Phase 5：Cloud Web Backend

实现与本地相同的：

```text
/api/state
/ws/telemetry
/api/system
/api/capabilities
```

然后直接部署同一套：

```text
dist/
```

---

## Phase 6：本地视频

```text
Video Pipeline
     ↓
Latest JPEG Frame
     ↓
webd
     ↓
Browser
```

---

## Phase 7：远程视频

需要时再增加：

```text
video-uplinkd
```

不要提前把它塞入 `telemetryd`。

---

# 32. 最终推荐进程架构

```text
                           T113
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
 terminal-core        video-service         logger
        │                   │
        │                   └── LatestFrame SHM
        │                            │
        │                            ▼
        │                          webd
        │                            │
        │                       Wi-Fi Browser
        │
        ├── VehicleState SHM
        │          │
        │    ┌─────┴─────────┐
        │    │               │
        │   webd         telemetryd
        │                    │
        └── Event Socket     │
                             │
                            4G
                             │
                             ▼
                         Cloud Server
                             │
                             ▼
                        Remote Browser
```

---

# 33. 最重要的模块边界

## `terminal-core`

```text
负责：
车辆

不负责：
网络
```

---

## `webd`

```text
负责：
本地浏览器访问

数据来源：
Local IPC
```

---

## `telemetryd`

```text
负责：
车辆 → 云服务器

数据来源：
Local IPC
```

---

## `cloud backend`

```text
负责：
云服务器 → 浏览器
```

---

## `dashboard`

```text
负责：
UI

不知道：
数据到底来自 T113 还是 Cloud
```

---

## `video-service`

```text
负责：
视频产生和录像
```

---

## `video-uplinkd`

```text
负责：
视频远程上传

第一阶段不存在也完全没有问题
```

---

# 34. 最终结论

整个系统最合理的关系不是：

```text
terminal-core
   ├── 本地 HTTP
   └── 公网 HTTP
```

而应该是：

```text
                    terminal-core
                         │
                    Local IPC Bus
                         │
              ┌──────────┴──────────┐
              │                     │
            webd               telemetryd
              │                     │
          Local Web                 4G
                                    │
                                 Cloud
                                    │
                               Remote Web
```

同时：

```text
                  dashboard source
                         │
                       build
                         │
              ┌──────────┴──────────┐
              │                     │
          T113 web root        Cloud web root
```

这样可以同时得到：

- 一套 VehicleState；
- 一套前端源码；
- 一套 API 数据模型；
- 两种访问通道；
- 本地和公网完全解耦；
- 网络问题不影响赛车核心；
- 后续可以自然增加 4G 视频；
- 也方便未来继续把现有单体 C 程序拆成成熟的 Linux 多进程系统。

在当前阶段，最值得先固定下来的不是 HTTP 库，而是三个接口：

1. `VehicleState` 结构与版本；
2. `terminal-core → local services` 的 IPC；
3. Local/Cloud 完全一致的 Web API schema。

这三项一旦定稳，`webd`、`telemetryd`、公网服务器和前端都可以并行开发，而不会互相绑死。
