# T113 开发板项目上下文总结

> 用途：给后续 Agent / Codex 直接接手开发使用。
> 更新时间：2026-08-15
> 当前阶段：开发环境和基础硬件接口验证完成，准备进入正式应用程序开发。

---

## 1. 项目目标

使用一块 **Allwinner T113 开发板**作为赛车数据终端 / 数据计算机。

预计后续主要功能包括：

- 读取整车 CAN
- 读取 BMS 内网 CAN
- 连接 LapPro 圈速设备蓝牙
- 串口 / RS485 外设通信
- 数据记录
- 圈速 / 遥测信息处理
- 后续可能连接 LCD / 触摸屏
- 最终作为车载长期运行的嵌入式 Linux 终端

开发原则：

- 优先工程可用、简单可靠
- 不追求桌面 Linux
- 尽量控制 RAM / Flash 占用
- 核心长期运行程序优先 C/C++
- Python 仅作为调试、协议验证辅助工具
- 日常开发通过 VS Code + WSL + 交叉编译 + SSH/SCP 部署

---

# 2. 硬件信息

## 2.1 T113 开发板

当前开发板：

- SoC：Allwinner T113
- 架构：ARMv7
- RAM：128 MB
- Flash：512 MB
- 板载 RTL8723 无线模块
- USB0 为 OTG，同时也是开发板供电口
- 两路 CAN 已由 Linux 注册
- 厂家提供 MIPI LCD / LVDS LCD 两个 Buildroot 镜像版本
- 当前使用：**LVDS Buildroot 镜像**
- 当前未购买 / 未连接 LCD

后续如果购买屏幕，倾向使用：

- LVDS
- 7 英寸左右
- 1024×600 或 1280×800

---

# 3. Linux 系统

厂家只提供 `.img` 镜像，没有 Buildroot SDK / BSP 源码。

目前使用厂家提供的：

**LVDS Buildroot 镜像**

没有使用 Ubuntu 20.04。

原因：

- 只有 128 MB RAM
- Buildroot 更适合最终车载嵌入式系统
- 厂家镜像包含板级驱动、设备树、U-Boot、无线、CAN 等支持
- 暂时不要自己换 Debian / Alpine / Armbian 等系统

---

# 4. 当前系统基本信息

执行：

```sh
uname -a
```

结果：

```text
Linux kickpi 5.4.61 #1 SMP PREEMPT Mon Jun 29 10:39:34 CST 2026 armv7l GNU/Linux
```

因此：

- Kernel：5.4.61
- 主机名：kickpi
- 架构：armv7l
- SMP 双核正常
- PREEMPT 内核

---

# 5. RAM 情况

```sh
free -m
```

结果：

```text
              total        used        free      shared  buff/cache   available
Mem:             99          27          59           0          12          67
Swap:             0           0           0
```

结论：

- 物理 RAM：128 MB
- Linux 可见 RAM：约 99 MB
- 系统启动后使用约 27 MB
- available 约 67 MB
- 没有 Swap

当前资源余量足以运行：

- CAN
- 蓝牙
- Wi-Fi
- SSH
- 数据记录
- C/C++ 主程序

---

# 6. Flash / 文件系统

```sh
cat /proc/mtd
```

结果：

```text
dev:    size   erasesize  name
mtd0: 00200000 00080000 "boot0"
mtd1: 00600000 00080000 "uboot"
mtd2: 00200000 00080000 "secure_storage"
mtd3: 1f600000 00080000 "sys"
```

大致：

- boot0：2 MB
- uboot：6 MB
- secure_storage：2 MB
- sys：约 502 MB

`df -h`：

```text
Filesystem                Size      Used Available Use% Mounted on
ubi0_5                  181.0M    141.1M     39.9M  78% /
tmpfs                    44.3M         0     44.3M   0% /dev/shm
tmpfs                    50.0M     64.0K     49.9M   0% /tmp
tmpfs                    50.0M    104.0K     49.9M   0% /run
devtmpfs                 44.3M         0     44.3M   0% /dev
/dev/by-name/UDISK      214.3M     24.0K    209.6M   0% /mnt/UDISK
```

当前建议：

- `/`：系统、程序、库、配置
- `/mnt/UDISK`：CAN log、圈速数据、遥测、CSV、数据库、运行日志

不要把大量数据写进 `/root`。

---

# 7. UDISK 持久化验证

已经执行测试：

```sh
echo "T113 storage test" > /mnt/UDISK/test.txt
sync
reboot
```

重启后：

```sh
cat /mnt/UDISK/test.txt
```

能够正常读取。

结论：

**`/mnt/UDISK` 重启后数据持久化正常。**

当前目录中曾存在：

```text
hello
t113_terminal
test.txt
```

注意：

在 Linux 当前目录执行程序需要：

```sh
./t113_terminal
```

不能直接：

```sh
t113_terminal
```

除非所在目录已经加入 `$PATH`。

---

# 8. USB / ADB

USB0 OTG：

- 同时用于开发板供电
- 烧录 / FEL
- Linux 启动后可作为 USB Gadget

最初看到：

```text
USB Device
VID_1F3A
PID_EFE8
```

这是 Allwinner FEL。

烧录 Buildroot 后正常冷启动需要等待一段时间，随后 Windows 会枚举：

```text
Configfs ffs gadget
```

最终确认：

**ADB 正常可用。**

Windows PowerShell 中可以：

```powershell
adb devices
adb shell
```

USB 启动比较慢，之前误以为冷启动失败，实际只是等待时间不足。

目前 ADB 可作为：

- 救援通道
- USB 调试接口
- shell
- 文件传输

---

# 9. Wi-Fi

无线模块驱动已经正常注册：

```text
wlan0
wlan1
```

`wlan0` MAC：

```text
68:8F:C9:10:05:7E
```

系统包含：

- `wpa_supplicant`
- `wpa_cli`
- `udhcpc`
- `iw`

厂家 Wi-Fi 启动脚本：

```text
/etc/init.d/S39wifi
```

内容核心逻辑：

```sh
MODULE_PATH="/lib/modules/5.4.61/8723bu.ko"
MODULE_NAME="8723bu"
```

启动时：

1. 加载 `8723bu.ko`
2. 启动：

```sh
wpa_supplicant -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf -B
```

因此不要重复手工启动第二个 `wpa_supplicant`，否则会报：

```text
ctrl_iface exists and seems to be in use
```

当前 Wi-Fi 已经连接成功。

配置文件：

```text
/etc/wpa_supplicant.conf
```

其中网络配置已经写入。

曾误加入：

```text
password="..."
```

已经删除，只保留：

```text
network={
    ssid="..."
    psk="..."
    key_mgmt=WPA-PSK
}
```

---

# 10. SSH

系统已有启动脚本：

```text
/etc/init.d/S50sshd
```

当前：

**SSH 已正常可用。**

开发板 Wi-Fi 联网后，可从 WSL / Windows 通过：

```bash
ssh root@T113_IP
```

连接。

文件上传：

```bash
scp xxx root@T113_IP:/mnt/UDISK/
```

日常开发倾向：

- USB：供电 + ADB 备用
- Wi-Fi：SSH / SCP 主开发通道

---

# 11. 蓝牙 RTL8723

Linux 已经识别蓝牙硬件：

```text
Bluetooth: hci0: RTL
hci_ver=06
lmp_subver=8723
```

蓝牙固件目录：

```text
/lib/firmware/rtl_bt/
```

文件实际存在：

```text
rtl8723b_config.bin
rtl8723b_fw.bin
```

但启动初期曾出现：

```text
Direct firmware load for rtl_bt/rtl8723b_fw.bin failed with error -2
firmware file rtl_bt/rtl8723b_fw.bin not found
```

初始：

```sh
hciconfig -a
```

显示：

```text
BD Address: 00:00:00:00:00:00
ACL MTU: 0:0
DOWN
Features: 全 0
```

手工执行：

```sh
hciconfig hci0 up
```

之后完全正常：

```text
BD Address: 68:8F:C9:10:05:7F
UP RUNNING
ACL MTU: 820:8
Name: 'RTK_BT_4.0'
HCI Version: 4.0
Manufacturer: Realtek Semiconductor Corporation
```

并且：

**蓝牙扫描正常，可以发现设备。**

因此问题判断为：

**蓝牙启动时序问题，而不是固件缺失。**

---

# 12. 蓝牙自动启动修复

厂家 `/etc/init.d` 原本有：

```text
S01syslogd
S02klogd
S10udev
S20mdev
S20urandom
S30dbus
S39wifi
S40network
S45ifplugd
S50adb_start
S50postgresql
S50powerkey_display
S50powerkey_suspend
S50sshd
S60audio_conf.sh
S60mount_udisk
S70launcher
S80bootplay
S80dnsmasq
S99swupdate_autorun
```

新增了：

```text
/etc/init.d/S55bluetooth
```

逻辑：

- 开机尝试 `hciconfig hci0 up`
- 如果失败，等待 1 秒重试
- 最多尝试约 10 次
- 避免简单写死单个 `sleep`

脚本大致为：

```sh
#!/bin/sh

case "$1" in
    start)
        echo "Starting Bluetooth..."

        i=0
        while [ $i -lt 10 ]; do
            if hciconfig hci0 up 2>/dev/null; then
                echo "Bluetooth hci0 is up"
                exit 0
            fi

            i=$((i + 1))
            sleep 1
        done

        echo "Failed to start Bluetooth hci0"
        ;;

    stop)
        hciconfig hci0 down 2>/dev/null
        ;;

    restart)
        "$0" stop
        sleep 1
        "$0" start
        ;;

    *)
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac

exit 0
```

并执行：

```sh
chmod +x /etc/init.d/S55bluetooth
```

重启验证：

**蓝牙已经可以正常自动启动。**

---

# 13. CAN

`ip link` 已经注册两路 CAN：

```text
awlink0
awlink1
```

而不是常见的：

```text
can0
can1
```

例如：

```text
2: awlink0
    link/can

3: awlink1
    link/can
```

第一路已经成功配置：

```sh
ip link set awlink0 up type can bitrate 500000
```

查看：

```sh
ip -details link show awlink0
```

结果：

```text
awlink0: <NOARP,UP,LOWER_UP,ECHO>
can state ERROR-ACTIVE
bitrate 500000
sample-point 0.875
berr-counter tx 0 rx 0
clock 24000000
```

结论：

- SocketCAN 正常
- 驱动正常
- 控制器正常
- 500 kbps 位时序正常
- CAN 状态 ERROR-ACTIVE 正常
- 当前 software/controller 层已经验证

项目计划：

- `awlink0`：整车 CAN
- `awlink1`：BMS 内网 CAN

后续正式程序直接使用 SocketCAN。

---

# 14. UART / GPIO / ADC 启动信息

启动日志中有：

```text
uart1: ttyS1
uart3: ttyS3
```

说明 UART1 / UART3 已注册。

存在以下 warning：

```text
/cpus/cpu@0 missing clock-frequency property
/cpus/cpu@1 missing clock-frequency property
```

暂时无需处理。

GPIO regulator：

```text
pio supply vcc-pg not found, using dummy regulator
pio supply vcc-pb not found, using dummy regulator
```

UART regulator：

```text
uart uart1: uart1 supply uart not found, using dummy regulator
uart uart3: uart3 supply uart not found, using dummy regulator
```

目前均不影响 UART 注册，暂时不处理。

GPADC：

```text
sunxi_gpadc_init ... success
sunxi_gpadc_setup: get channel scan data failed
```

ADC 驱动初始化成功，但通道扫描配置可能不完整。

目前不是项目优先事项。

---

# 15. Python

目标板只有：

```text
Python 3.7.2
```

没有板端 gcc / g++。

当前决定：

- 不在 T113 上本地编译
- PC 交叉编译
- Python 仅用于快速协议验证 / 临时工具
- 正式长期运行程序优先 C/C++

---

# 16. glibc / ABI

目标板：

```sh
/lib/libc.so.6
```

输出：

```text
GNU C Library (GNU libc) stable release version 2.34.
```

动态链接器：

```text
/lib/ld-linux-armhf.so.3
```

因此目标 ABI：

- ARMv7
- 32-bit ARM
- hard-float
- armhf
- glibc 2.34

交叉编译器：

```text
arm-linux-gnueabihf-gcc
```

已经验证可以生成目标板正常运行的 ELF。

注意：

不要直接在 T113 上替换 / 升级 glibc。

如果未来需要升级 libc，应该重新构建整个 Buildroot rootfs，而不是手工替换：

```text
libc.so.6
ld-linux-armhf.so.3
```

---

# 17. WSL 开发环境

Windows 已安装：

- WSL
- Ubuntu 22.04
- `arm-linux-gnueabihf-gcc`

虽然 Ubuntu 22.04 host glibc 是 2.35，但已经实际验证交叉编译出的简单目标程序可在 T113 glibc 2.34 上正常运行。

后续如果引入更多依赖，建议检查：

```bash
arm-linux-gnueabihf-readelf --version-info build/xxx | grep GLIBC
```

确保最高 GLIBC requirement 不超过目标板 2.34。

---

# 18. VS Code

当前使用：

**Windows VS Code UI + WSL workspace**

也就是从 WSL：

```bash
code .
```

打开工程。

注意：

这不是在 WSL 里运行另一套 VS Code GUI，而是：

```text
Windows VS Code
    +
WSL VS Code Server
```

当前环境：

- VS Code Windows
- WSL extension
- C/C++ extension
- CMake Tools
- WSL 中 ARM GCC

项目已经成功在 VS Code CMake 中编译。

---

# 19. 当前 CMake / Toolchain 方案

推荐工程：

```text
t113-terminal/
├── CMakeLists.txt
├── toolchain-t113.cmake
├── src/
│   └── main.c
├── include/
├── build/
└── .vscode/
```

`toolchain-t113.cmake`：

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
```

基础 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)

project(t113_terminal
    VERSION 0.1.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(t113_terminal
    src/main.c
)
```

目前：

**VS Code CMake 已经成功跑通。**

---

# 20. 交叉编译链验证

已经成功完成：

```text
WSL
  ↓
arm-linux-gnueabihf-gcc
  ↓
ARM ELF
  ↓
scp / SSH
  ↓
T113
  ↓
正常运行
```

测试程序：

```c
#include <stdio.h>

int main(void)
{
    printf("Hello from T113!\n");
    return 0;
}
```

已经成功上板运行。

因此以下链路已经确认：

**PC 交叉编译 → ARMHF ELF → T113 glibc 2.34 → 正常执行**

---

# 21. VS Code 远程部署 / 调试

当前已经确认：

**VS Code 调试环境正常，远程部署正常。**

推荐架构：

```text
launch.json
    ↓
preLaunchTask
    ↓
tasks.json
    ├── CMake build
    ├── SCP 上传
    ├── SSH
    └── 启动目标程序 / gdbserver
```

原则：

- `tasks.json` 负责：
  - 编译
  - scp
  - ssh
  - 启动远程程序
- `launch.json` 负责：
  - GDB 调试
  - 断点
  - 单步
  - Watch
  - Call Stack

如果使用远程 GDB：

目标板：

```sh
gdbserver :2345 /mnt/UDISK/t113_terminal
```

WSL：

```text
gdb-multiarch
```

`launch.json` 可以使用：

```json
"miDebuggerServerAddress": "T113_IP:2345"
```

本地 `program` 应指向带调试符号的本地 ELF：

```text
${workspaceFolder}/build/t113_terminal
```

而不是远端文件路径。

---

# 22. 当前已经验证完成的项目

## 基础系统

- [x] Buildroot 可正常启动
- [x] 冷启动正常
- [x] USB Gadget 正常
- [x] ADB 正常
- [x] RAM 状态确认
- [x] Flash 分区确认
- [x] UDISK 持久化确认

## 网络

- [x] RTL8723 Wi-Fi 驱动正常
- [x] wlan0 可用
- [x] Wi-Fi 正常连接
- [x] SSH 正常

## 蓝牙

- [x] RTL8723 Bluetooth 硬件识别
- [x] firmware 文件存在
- [x] hci0 可正常 UP
- [x] 扫描蓝牙设备正常
- [x] 修复开机蓝牙初始化时序
- [x] 开机自动启动正常

## CAN

- [x] awlink0 注册
- [x] awlink1 注册
- [x] SocketCAN 可配置
- [x] 500 kbps 配置成功
- [x] CAN controller 状态正常

## 开发环境

- [x] WSL Ubuntu 22.04
- [x] arm-linux-gnueabihf-gcc
- [x] ARMHF ELF 上板运行
- [x] VS Code WSL
- [x] CMake
- [x] SSH / SCP 自动部署
- [x] VS Code 远程调试链路

---

# 23. 当前未完成 / 后续任务

## 23.1 LapPro 蓝牙

当前手边没有 LapPro 盒子。

预计几天后到手。

到手后需要：

1. 扫描设备
2. 确认 Classic Bluetooth 或 BLE
3. 如果 BLE：
   - 扫描 GATT Service
   - 枚举 Characteristic
   - 找 Notify / Indicate
   - 抓取原始数据
4. 对比 LapPro 手机 App 行为
5. 反向分析圈速 / GPS / 状态数据协议
6. 最终集成到 T113 主程序

---

## 23.2 正式应用程序

下一步建议正式进入主程序开发。

第一版建议只实现：

```text
awlink0
   ↓
SocketCAN
   ↓
接收 CAN frame
   ↓
时间戳
   ↓
打印
```

然后逐步扩展：

```text
awlink0 + awlink1
        ↓
双 CAN 输入
        ↓
统一内部消息结构
        ↓
数据缓存
        ↓
日志写入
        ↓
/mnt/UDISK
```

再后续加入：

```text
Bluetooth / LapPro
UART / RS485
LCD
触摸
数据展示
配置
异常管理
```

---

# 24. 推荐的软件架构方向

目前不要过度设计。

第一阶段建议：

```text
main
│
├── can.c / can.h
├── logger.c / logger.h
├── bluetooth.c / bluetooth.h
├── config.c / config.h
└── main.c
```

稍后再决定：

- pthread
- epoll
- ring buffer
- producer/consumer
- SQLite / CSV / 自定义二进制 log

目前先优先：

**功能跑通 > 架构复杂度**

---

# 25. 重要约束

后续 Agent 开发时必须记住：

1. 目标板只有 **128 MB RAM**
2. Flash 只有 **512 MB**
3. 根分区 `/` 只剩约 **40 MB**
4. 大量运行数据必须写 `/mnt/UDISK`
5. 不要随意替换 glibc
6. 不要随意刷写：
   - boot0
   - uboot
   - secure_storage
7. 厂家没有 SDK，只有 `.img`
8. 当前原厂系统已经稳定工作，修改系统需保守
9. 日常开发以 SSH / SCP 为主
10. ADB 作为 USB 救援 / 调试通道
11. CAN 接口名是：
    - `awlink0`
    - `awlink1`
12. 蓝牙设备：
    - `hci0`
13. Wi-Fi：
    - `wlan0`
14. 数据目录：
    - `/mnt/UDISK`
15. 目标编译器：
    - `arm-linux-gnueabihf-gcc`
16. 目标 glibc：
    - 2.34
17. 目标动态链接器：
    - `/lib/ld-linux-armhf.so.3`

---

# 26. 当前建议的下一步

不要继续做 Hello World 或单纯验板。

直接开始正式程序：

## Phase 1

实现：

```text
SocketCAN receiver
```

功能：

- 打开 `awlink0`
- 设置 CAN 500 kbps 可由启动脚本预先完成
- 接收 CAN frame
- 输出：
  - 时间戳
  - CAN ID
  - DLC
  - DATA
- 正常处理 SIGINT
- 可以长期运行

## Phase 2

加入：

```text
awlink1
```

形成双 CAN。

## Phase 3

加入日志：

```text
/mnt/UDISK/log/
```

先使用简单二进制或 CSV。

## Phase 4

LapPro 到手后加入蓝牙模块。

---

# 27. 给 Agent 的工作方式建议

如果 Agent 可以操作当前 Windows / WSL 环境，应优先直接执行：

```bash
cmake --build build
scp ...
ssh ...
```

自行：

- 修改源码
- 编译
- 上传
- 启动
- 查看日志
- 修复
- 重复验证

尽量减少人工复制命令。

但禁止 Agent 未经确认执行高风险操作，例如：

```text
flash / 烧录
擦除 MTD
格式化 UBI
修改 boot0
修改 U-Boot
替换 glibc
删除 UDISK 数据
```

---

# 28. 当前项目状态一句话总结

**T113 原厂 Buildroot 已稳定运行，ADB、Wi-Fi/SSH、蓝牙、双 CAN、UDISK 持久化和 VS Code/WSL ARMHF 交叉编译与远程部署调试链路均已跑通，现在应直接进入 SocketCAN 数据终端主程序开发。**
