# Bluetooth/L2CAP Hang Investigation

日期：2026-08-29
平台目标：Allwinner T113-S3 / ARMv7 / Linux 5.4.61 PREEMPT / RTL8723 系列

## 结论摘要

当前证据把问题从“视频链路内存或 G2D/Cedar 故障”收敛到了 Bluetooth L2CAP
socket teardown。项目使用的是 native LE L2CAP fixed CID 4 socket，不是 BlueZ
D-Bus 或 `gatttool`。

本轮已完成的低风险修复是：

1. LapPro connection socket 的生命周期由单一 `lappro-bt` worker 负责。
   `lappro_stop()` 只设置停止标志并 `join`，不再从主线程对 worker 正在收包的
   fd 调用 `shutdown()`。
2. 成功连接的 fd 只在 worker 的统一 cleanup 点执行一次
   `shutdown(SHUT_RDWR)` + `close()`；连接失败时的临时 fd 也只由该 worker
   清理。
3. 增加 `DISCONNECTED/CONNECTING/CONNECTED/DISCONNECTING/BACKOFF/STOPPING`
   状态、connection generation，以及带 monotonic 时间、tid、线程名、generation
   和 fd 的低频生命周期日志。
4. 重连退避改为 1 s、2 s、5 s 上限，禁止重叠 connect/disconnect。
5. 保存两个与当前 5.4.61 调用栈高度匹配的 upstream kernel patch，待取得实际
   vendor kernel tree 后检查是否已包含并进行最小回移植。

这还不是“已在 ARM 板端根治”的结论：本轮板端 SSH
`root@192.168.34.238:22` 超时，UART/A-B/长稳测试均未能执行。不能用源码静态分析
替代板端复现。

## 1. 原始 UART 证据

本轮依据的原始信息如下，未做删改：

```text
rcu: INFO: rcu_preempt self-detected stall on CPU
```

```text
CPU: 1 PID: 35 Comm: kworker/1:1
Workqueue: events l2cap_chan_timeout
```

```text
arch_spin_lock
    ↓
lock_sock_nested
    ↓
l2cap_sock_teardown_cb
    ↓
l2cap_chan_del
    ↓
l2cap_chan_close
    ↓
l2cap_chan_timeout
    ↓
process_one_work
    ↓
worker_thread
```

```text
rcu: INFO: rcu_preempt self-detected stall on CPU
1-....: (1 GPs behind)

Workqueue: events l2cap_chan_timeout

[<c015062c>] arch_spin_lock
[<c067e57c>] lock_sock_nested
[<c07b5608>] l2cap_sock_teardown_cb
[<c07ad384>] l2cap_chan_del
[<c07ad5e4>] l2cap_chan_close
[<c07ad674>] l2cap_chan_timeout
```

这个 stack 直接证明 CPU1 在 L2CAP channel timeout 的 socket teardown 路径上
长时间进入 `lock_sock_nested()`。它证明了 teardown 是 stall 的现场，但单凭此
stack 还不能区分 socket lock 长时间竞争、socket 生命周期 race 后的锁对象损坏，
还是 vendor kernel 的额外差异。

## 2. A/B 隔离矩阵

### Case A：Bluetooth OFF

状态：**未执行，BLOCKED（板端 SSH 不可达）**。

本轮探测结果：

```text
ssh: connect to host 192.168.34.238 port 22: Connection timed out
```

板端恢复后执行一次以下步骤，UART 从测试开始前持续保存：

```bash
dmesg -n 8
killall bluetoothd 2>/dev/null
hciconfig hci0 down
rfkill block bluetooth 2>/dev/null || true
hciconfig
ps | grep -E 'bluetooth|lappro'
```

随后运行原来可以在 30--40 s 触发 hang 的：

```text
Camera + Cedar + G2D + PNG HUD + TTF
```

不录像、不访问 U 盘，持续至少 120 s，记录：运行时间、最后一条 UART、heartbeat、
SSH、RCU/soft-lockup/kernel warning。当前没有 A 的实测结果，因此不能宣称
“Bluetooth OFF 后稳定”。

### Case B：Bluetooth-only

状态：**未执行，BLOCKED（同一板端连接问题）**。

关闭 Camera、Cedar、G2D、HUD、Encoder、MP4 Writer，仅运行 LapPro native L2CAP
连接和 notification/data receiving，持续至少 120 s。按照真实使用节奏完成一次：

```text
正常连接
一次正常 disconnect/reconnect
一次被动断开或短暂信号丢失
```

不要使用几十毫秒级疯狂重连。UART 重点检索：

```text
l2cap_chan_timeout
l2cap_sock_teardown_cb
lock_sock_nested
RCU stall
Bluetooth: hci0
RTL
command tx timeout
hardware error
unexpected event
```

### A/B 当前判定

| 项目 | 结果 | 说明 |
|---|---|---|
| A，Bluetooth OFF + 视频 HUD，120 s | 未执行 | 目标板 SSH 超时 |
| B，Bluetooth-only，120 s | 未执行 | 目标板 SSH 超时 |
| 原始 trace 是否落在 L2CAP teardown | 已确认 | UART stack 明确给出 `l2cap_chan_timeout` |
| 视频链路是否已被证明为根因 | 否 | 当前优先级低于 Bluetooth/L2CAP |

## 3. 当前 LapPro 实现与生命周期

源代码位于 [`core/src/lappro.c`](core/src/lappro.c) 和
[`core/include/lappro.h`](core/include/lappro.h)。静态审查结果：

- `lappro_connect()` 创建 `socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)`，
  绑定 LE ATT fixed CID 4，并连接目标地址；没有发现 BlueZ D-Bus、gatttool 或
  另一套 Bluetooth manager。
- 项目只有一个 LapPro pthread。连接后的 MTU exchange、CCCD write、认证和
  notification receive 都在这个 worker 中进行。
- 原实现的危险序列是：主线程 `lappro_stop()` 设置停止标志后，直接对
  `active_fd` 调用 `shutdown()`；worker 可能同时位于 `recv()` 或内核 timeout
  teardown，随后 worker 自己再调用 `shutdown()` 和 `close()`。
- 原实现没有 overlapping connect thread，但 stop/disconnect/reconnect 的 fd
  操作跨线程，且同一连接的 teardown 可能由用户 shutdown 和 kernel timeout 同时
  进入。

修复后的生命周期是：

```text
DISCONNECTED
  -> CONNECTING [worker 创建/连接 fd]
  -> CONNECTED  [worker 完成 connect]
  -> DISCONNECTING [worker 收包结束或 stop 被观察到]
  -> DISCONNECTED [worker shutdown + close + 清除 active_fd]
  -> BACKOFF [1 s / 2 s / 5 s]
  -> CONNECTING
```

程序退出时现在是：

```text
主线程设置 stop_requested + STOPPING
  -> worker 停止 recv/connect polling
  -> worker 统一 shutdown/close
  -> 主线程 pthread_join
  -> 释放 LapPro mutex/state
```

`core/src/lappro.c` 中仍有多个 `close()`，但失败路径和成功路径都属于同一个
worker 的连接 owner；`lappro_stop()` 不再直接触碰连接 fd。这消除了当前应用层
最明确的跨线程 shutdown/close 交叉点。

## 4. Linux 5.4.61 L2CAP 锁关系

对照的源码是 Linux 5.4.61 的
[`net/bluetooth/l2cap_core.c`](https://github.com/gregkh/linux/blob/v5.4.61/net/bluetooth/l2cap_core.c)
和
[`net/bluetooth/l2cap_sock.c`](https://github.com/gregkh/linux/blob/v5.4.61/net/bluetooth/l2cap_sock.c)。

### Timeout worker 路径

5.4.61 的 `l2cap_chan_timeout()` 先取得 `conn->chan_lock`，再取得 channel lock，
并调用 `l2cap_chan_close()`。对需要删除的 channel，
`l2cap_chan_del()` 在 channel lock 上下文中执行 socket ops 的 teardown callback，
即：

```text
conn->chan_lock
  -> chan->lock
    -> l2cap_chan_close()
      -> l2cap_chan_del()
        -> l2cap_sock_teardown_cb()
          -> lock_sock_nested(sk)
            -> sk->sk_lock.slock / socket lock
```

因此原始 stack 中等待的 socket lock 并不是视频 ioctl 锁；它位于 Bluetooth
channel timeout 到 socket teardown 的最后一段。

### 用户 shutdown/release 路径

5.4.61 的 `l2cap_sock_shutdown()` 会先对 socket `lock_sock(sk)`，完成 socket
层状态处理后释放；随后取得 channel/connection 的锁并调用
`l2cap_chan_close()`，最后再次取得 socket lock 完成 shutdown。`close()` 则通过
`l2cap_sock_release()` 进入 shutdown、`sock_orphan()` 和 `l2cap_sock_kill()`。

这意味着下列两条路径可以同时针对同一 channel/socket 做 teardown：

```text
Thread A: l2cap_chan_timeout -> l2cap_chan_close -> l2cap_chan_del
             -> l2cap_sock_teardown_cb -> lock_sock_nested

Thread B: shutdown(fd) -> l2cap_sock_shutdown -> l2cap_chan_close
             -> l2cap_sock_teardown_cb -> lock_sock_nested

Thread C: close(fd) -> l2cap_sock_release -> sock_orphan
             -> l2cap_sock_kill -> socket free
```

当前 trace 不能仅凭地址证明是 ABBA；更符合源码和 upstream 报告的判断是：

1. 应用主线程主动 shutdown 让 Thread B 与 worker/kernel timeout 的 teardown
   重叠概率增加。
2. 5.4.61 的 `l2cap_sock_teardown_cb()` 在进入 cleanup 前设置 `SOCK_ZAPPED`，
   存在与 release/free 交错的已知 socket 生命周期 race。
3. 如果 socket 已被释放而 timeout callback 仍从 `chan->data` 取得旧 socket，
   随后在 `lock_sock_nested()` 看到损坏的 socket lock，就可能表现为 spinlock
   异常、无限等待或 RCU stall。

所以本轮将“应用层跨线程 shutdown”作为高置信度放大器，将“5.4.61 L2CAP socket
teardown race”作为高匹配度内核缺陷；在没有 Case B 和板端新 trace 前，不把它们
表述为已经完成的唯一根因证明。

## 5. Upstream 修复检索

| commit | upstream/stable 信息 | 与当前 trace 的关系 |
|---|---|---|
| `20ae4089d0afeb24e9ceb026b996bfa55c983cc2` | `Bluetooth: Only mark socket zapped after unlocking`；已进入 5.4.73 stable queue | 修复 `l2cap_sock_teardown_cb()` 在 cleanup 前设置 `SOCK_ZAPPED` 与 `l2cap_sock_release()` free 的 race；与当前 socket teardown/spinlock 症状高度相关 |
| `1bff51ea59a9afb67d2dd78518ab0582a54a472c` | `Bluetooth: fix use-after-free error in lock_sock_nested()`；后续进入 stable backport | upstream 报告的 workqueue 正是 `events l2cap_chan_timeout`，stack 包含 `lock_sock_nested -> l2cap_sock_teardown_cb -> l2cap_chan_del -> l2cap_chan_close -> l2cap_chan_timeout`，并明确描述 timeout、shutdown、close 三条并发路径 |

参考：[`20ae` stable patch](https://kernel.googlesource.com/pub/scm/linux/kernel/git/stable/stable-queue/+/refs/tags/v6.12.41/releases/5.4.73/bluetooth-only-mark-socket-zapped-after-unlocking.patch)，
[`20ae` 原始讨论](https://lists.openwall.net/netdev/2020/09/11/279)，以及
[`1bff` stable 讨论及完整 stack](https://lists.openwall.net/netdev/2021/11/09/82)。

仓库新增：

- [`0002-bluetooth-only-mark-socket-zapped-after-unlocking.patch`](kernel_patches/0002-bluetooth-only-mark-socket-zapped-after-unlocking.patch)
- [`0003-bluetooth-fix-l2cap-socket-teardown-use-after-free.patch`](kernel_patches/0003-bluetooth-fix-l2cap-socket-teardown-use-after-free.patch)

这两个文件是最小回移植候选，不代表已经写入板端 kernel。拿到 vendor kernel
tree 后必须先执行：

```bash
git apply --check kernel_patches/0002-bluetooth-only-mark-socket-zapped-after-unlocking.patch
git apply --check kernel_patches/0003-bluetooth-fix-l2cap-socket-teardown-use-after-free.patch
```

若 vendor tree 已包含相同逻辑，应跳过重复 patch；若上下文有差异，只做对应
5.4.61 vendor tree 的等价最小 backport，并保存编译后的 kernel/modules 配置和
commit 信息。

## 6. HCI/RTL8723 检查状态

应用源码中没有直接的 RTL8723 transport 实现；LapPro 只使用 kernel Bluetooth
socket。由于板端不可达，本轮没有取得死机前的：

```text
hci0 disconnect event
hardware error
HCI command timeout
unexpected event
RTL firmware/transport log
```

因此 RTL8723/HCI 不能排除，但当前优先级仍低于已出现的
`l2cap_chan_timeout -> l2cap_sock_teardown_cb` 直接证据。板端复测时应保存：

```bash
uname -a
cat /proc/version
hciconfig -a
dmesg -n 8
dmesg | grep -Ei 'Bluetooth|L2CAP|hci0|RTL|timeout|hardware error|unexpected event'
```

如系统带 `btmon`，只在 Bluetooth-only 和完整链路测试中各保存一份，避免持续高
强度抓包改变时序。

## 7. 修复内容与预期效果

### 用户态

`lappro_stop()` 现在只负责发布停止请求和等待 worker。由于收包和发包使用 100 ms
socket timeout、connect 使用 100 ms polling，worker 能在有界时间内观察停止标志。
worker 完成收包/认证/通知循环后，才执行统一的：

```text
DISCONNECTING
  -> state.connected = false
  -> worker-only shutdown(fd)
  -> worker-only close(fd)
  -> active_fd = -1
  -> DISCONNECTED
```

这样可以移除主线程 `shutdown(fd)` 与 worker 的 `recv()`/close 之间最明确的
并发操作；generation 日志可以确认旧连接是否已退出后才产生下一条 CONNECTING。
退避则降低异常断开时连续进入 kernel L2CAP connect/timeout teardown 的频率。

### Kernel 候选

`20ae` 把 `SOCK_ZAPPED` 移到 socket cleanup 和 `release_sock()` 之后；`1bff`
让 teardown/close callback 对已清空的 `chan->data` 安全返回，并在 socket destructor
中先清空 `chan->data`。它们分别覆盖已知 zapped/free race 和
`lock_sock_nested()` UAF race。

本轮没有对 Bluetooth 子系统做大面积重构，没有关闭 Bluetooth，没有改变 RCU
watchdog，也没有通过增大 timeout 掩盖问题。

## 8. 验证记录

### 已完成的主机侧检查

```text
arm-linux-gnueabihf-gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only ... core/src/lappro.c
PASS
```

`git diff --check`：PASS。

两个 kernel patch 已在干净的 upstream `v5.4.61` 源码检出上顺序执行
`git apply --check`：PASS（先应用 `0002`，再检查 `0003`）。这只证明它们能匹配
upstream 5.4.61，不证明 vendor kernel 没有冲突。

完整 CMake 构建：**BLOCKED**。仓库重新配置时报告：

```text
FreeType development files are required for the T113 HUD.
Set T113_FREETYPE_INCLUDE_DIR and T113_FREETYPE_LIBRARY
```

这是当前构建环境缺少 HUD 既有 FreeType host/sysroot 依赖，未发现本次 LapPro
代码的编译错误。配置 FreeType 后应重新执行：

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

本轮直接运行现有 `build` 目录的 CTest 得到 1/15 PASS、14/15 FAIL；失败测试的
可执行文件被 shell 报为 `Syntax error`，符合 build 目录中已有目标与当前主机/目标
架构不匹配或旧构建产物的特征，不是 LapPro 测试失败。应在 FreeType 配置完成后
清理并按当前工具链重新 configure/build，再复测。

### ARM 板端结果

| 指标 | 结果 |
|---|---|
| Case A 120 s | 未执行，SSH timeout |
| Case B 120 s | 未执行，SSH timeout |
| Bluetooth-only 修复后 120 s | 未执行 |
| Bluetooth + Camera + Cedar 180 s | 未执行 |
| 完整 HUD、无录像 5 min | 未执行 |
| 完整正式链路 10 min | 未执行 |
| 修复后 RCU stall | 未知 |
| 修复后 heartbeat freeze | 未知 |
| 修复后 SSH freeze | 未知 |
| frame/drop/RSS/ION/DMA/MP4 | 未执行 |

## 9. 修复后的板端执行顺序

所有测试都必须接 UART，并在测试开始前执行 `dmesg -n 8`：

1. Bluetooth-only + LapPro，至少 120 s，包含一次正常 disconnect/reconnect。
2. Bluetooth + Camera + Cedar，至少 180 s。
3. Camera + Cedar + G2D + PNG HUD + TTF + Bluetooth/LapPro，不录像、不写 U 盘，
   至少 5 min。
4. 完整 Camera + Cedar + G2D + HUD + TTF + Bluetooth + CAN + Encoder + Writer +
   MP4，至少 10 min。

每个阶段保留：

```text
UART 全量日志
frame count / dropped frames
CPU / RSS / ION-DMA
Bluetooth reconnect count
kernel warnings / RCU stall / soft lockup
heartbeat
MP4 完整性
```

PASS 条件是无 RCU stall、无 soft lockup/kernel warning、heartbeat 正常，并且
完整链路的 MP4 可正常关闭和解析。若再次出现 stall，优先用新的
`[BT][...][gen=...][fd=...]` 日志对齐 UART 中的 `hci0`、断开事件和 timeout 时间。

## 10. 尚未排除的风险

- A/B 尚未在实际 T113-S3 上执行，Bluetooth 与 hang 的因果关系仍需实测确认。
- vendor Linux 5.4.61 可能已经私自包含其中一个 patch，也可能对
  `l2cap_sock.c` 有不同 backport；不能直接假设补丁能无冲突应用。
- 当前用户态修复降低并发 teardown，但不能单独修复 kernel timeout 与 socket
  release 的所有内部 race；若 B 仍复现，应启用并验证两个 upstream kernel patch。
- RTL8723 firmware、HCI transport、断开事件和 hardware error 尚未取得现场日志。
- 完整 CMake/ctest 和 ARM 运行时验证因 FreeType 配置与板端连接分别受阻。
- 本轮没有证明 Cedar/G2D/ION/MP4 无关，只是依据当前更直接的 L2CAP trace 将它们
  从第一嫌疑降级；若 Bluetooth OFF 的 Case A 仍挂，需要重新建立视频侧证据链。
