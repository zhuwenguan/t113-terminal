# G2D / ION / VE teardown P0 修复与实板验证（2026-08-25）

## 结论

**P0 FIX VERIFIED**

- 生产修复提交：`d003f8e fix(video): bracket G2D buffers within VE lifetime`
- 故障注入提交：`b3d76a3 test(video): inject lifecycle initialization failures`
- T113 被测生产二进制 SHA-256：
  `05c5f48973b812a04526e672a7607baa069055687d4062cd9f8a38128af790d5`
- 本轮未修改分辨率、正常视频数据流、queue/drop 策略或性能参数，也未处理 P1/P2。

## 泄漏根因

`libawion.so` 的 `sunxi_ion_alloc_palloc()` 不只是分配、mmap 和返回 dma-buf
fd；它在分配时还会在自己的 `/dev/cedar_dev` fd 上发
`GET_IOMMU_ADDR (0x502)`，把每块 buffer attach 到 `1c0e000.ve`。

旧生产顺序为：

```text
compositor palloc/0x502
→ 首次 decoder/encoder VeInitialize
→ video session
→ encoder/decoder Destroy/VeRelease
→ compositor pfree/0x503
```

这同时破坏 IOMMU 生命周期的两端：首次 `VeInitialize` 发生在 attachment
建立之后，最后一个 VE session 又在 `pfree` 之前释放。板端驱动随后让
`pfree` 的 `0x503` 返回成功，但没有真正解除旧 attachment；用户态 wrapper、
mmap 和 fd 已释放，底层 dma-buf 仍由 `1c0e000.ve` 持有。

泄漏对象与 compositor allocation 精确对应：

| Buffer | 数量 | 单个 dma-buf 实测字节 | 合计字节 |
|---|---:|---:|---:|
| YUV420 staging | 1 | 1,384,448（页对齐） | 1,384,448 |
| ARGB camera/HUD/final | 3 | 3,686,400 | 11,059,200 |
| **总计/session** | **4** | | **12,443,648** |

实板 2×2 A/B 证明这是双向约束：错误 init + buffers-first teardown 仍泄漏；
正确 init + encoder-first teardown 仍泄漏；只有下列完整窗口回到基线：

```text
首次 VeInitialize 成功
→ libawion palloc/0x502
→ G2D / encoder 运行
→ libawion pfree/0x503
→ 最后 VideoEncDestroy / DestroyVideoDecoder / VeRelease
```

精确反汇编还确认：

- `sunxi_ion_alloc_pfree()` 已执行 `munmap → 原 cedar fd 上 0x503 → close dma fd`；
- `VideoEncDestroy()` 内部已经调用 `VideoEncUnInit()`；
- 另开 `/dev/cedar_dev` 发 `0x503/0x505` 不具有原 attachment 的 filp
  ownership，即使返回 0 也可能是 no-op，因此不是安全修复。

## 资源 ownership

| Resource | Created by | Owner | Borrower | Released by | Failure cleanup |
|---|---|---|---|---|---|
| 进程级 VE session | native decoder / encoder vendor API | libVE 全局引用 | decoder、encoder、libawion IOMMU | 最后 `Destroy*` / `VeRelease` | decoder/encoder close；必须晚于 compositor pfree |
| libawion context | `sunxi_ion_alloc_open` | `VideoCompositor` | 四个 ION node | `sunxi_ion_alloc_close` | `video_compositor_close` |
| 1×YUV + 3×ARGB | `sunxi_ion_alloc_palloc` | libawion node / compositor | G2D、HUD、final frame | 每块 `sunxi_ion_alloc_pfree` | partial init 也逐块检查并释放 |
| mmap | libawion palloc | libawion node | CPU/HUD/G2D staging | pfree 内部 `munmap` | pfree |
| dma-buf fd | libawion palloc | libawion node | G2D ioctl | pfree 内部 `close` | pfree |
| VE IOMMU attachment | palloc 内部 `0x502` | 原 cedar fd/filp | G2D/VE | pfree 内部同一 fd `0x503` | pfree，且 VE session 必须仍存活 |
| `/dev/g2d` fd | compositor | compositor | 同步 G2D ioctl | compositor close | init/stop cleanup |
| HUD external surface | compositor `argb_hud` | compositor | `HudRenderer` 只借用 | compositor pfree | HUD 先停止借用，不自行 free |
| native decoded picture | decoder | decoder | video worker 一帧内借用 | `ReturnPicture` | worker join/decoder close |
| encoder input | Cedar encoder | encoder | producer 一帧内借用 | return/release/Destroy | recorder close |
| encoded JPEG output | `VideoEncoder.output` heap | encoder | enqueue 前临时借用 | encoder close | queue push 已复制 payload |
| writer queue slot | `EncodedFrameQueue` | recorder | writer thread | writer release/destroy | finalize close + join |
| MP4/idx `FILE *` | writer | writer/mux | 无外借 | finalize/close | writer error cleanup |

## 实际修改与正确 teardown

`video_pipeline_init()` 现在先建立 native decoder VE session，再初始化 G2D
compositor，最后初始化 recorder/encoder。native decoder 不可用时，explicit
G2D 在任何 palloc 前失败，AUTO 在任何 palloc 前转为 CPU；mock 非 CPU 配置也在
palloc 前拒绝（主程序 HUD mock 本来就强制 CPU）。

`video_pipeline_stop()` 的顺序为：

```text
request stop → join video producer
→ recorder finalize / drain writer queue（encoder仍存活）
→ close capture / borrowed HUD state
→ close G2D fd
→ pfree 3×ARGB + 1×YUV
→ ion allocator close
→ encoder close / Destroy
→ decoder close / Destroy
→ mutex destroy
```

JPEG payload 在 enqueue 时已复制到独立 queue slot，所以 writer drain 不持有
compositor 或 encoder output。另增加 `compositor.g2d_fd = -1` partial-init
sentinel，避免 decoder/init early return 误关 fd 0。

独立诊断工具也改成同一生命周期窗口，并删除另开 cedar cleanup fd、
`ENGINE_REQ/REL` 和 raw `0x503/0x505` 的伪清理。

## Fresh boot 与 20 轮生产 start/stop

fresh boot baseline：

- dma-buf：`1 object / 4,915,200 bytes`（display only）
- ION：`1 buffer / 4,915,200 bytes`
- VE/G2D `clk_enable_count`：`0 / 0`

每轮运行真实 USB MJPEG → Cedar decode → G2D HUD → Cedar encode → async writer，
录像 10 秒，stop 后等待 1 秒再读取 debugfs：

| Round | dma-buf objects | dma-buf bytes | ION buffers | ION bytes | VE/G2D clock | Result |
|---:|---:|---:|---:|---:|---:|---|
| 1 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 2 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 3 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 4 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 5 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 6 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 7 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 8 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 9 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 10 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 11 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 12 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 13 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 14 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 15 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 16 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 17 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 18 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 19 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |
| 20 | 1 | 4,915,200 | 1 | 4,915,200 | 0/0 | PASS |

20 个 MP4 均有 `moov`，PTS 单调，JPEG marker/sample 数全量匹配，无残留
`.idx`。每轮 254--281 帧、duration 9.487--9.551 秒；该 FPS 波动是既有 720p
性能问题，不属于本轮修改。

运行中最大 RSS 约 8.6--10.6 MiB（debug resource snapshot），fd peak 24；
每个进程退出后无残留 pid/thread/fd。低层 E/F pipeline 另在同一 fresh boot
连续运行 20 轮，每轮 `dma_buf_count_delta=0`、`dma_buf_bytes_delta=0`、
`fd_delta=0`。

## Teardown 边界与故障注入

| 场景 | 实测行为 | stop 后资源 |
|---|---|---|
| start 后首帧立即 stop，连续 10 轮 | 10/10 rc=0 | 每轮 DMA/ION baseline，VE/G2D 0/0 |
| G2D open 失败（四块 palloc 后） | pipeline init 按预期失败，无 crash | baseline，VE/G2D 0/0 |
| `/dev/ion` open 失败 | 无 DMA 残留；全局注入同时破坏 Cedar allocator，厂商报 refcount error | 不作为精确单模块结论；见限制 |
| decoder 已成功，encoder `dlopen` 失败 | 仅 recording disabled，G2D video 继续 | baseline，VE/G2D 0/0 |
| Writer/mux 初始化失败（目标已存在） | 仅 recording disabled，video 继续 | baseline，VE/G2D 0/0 |
| 正常 20×10 s stop | queue drain、MP4 finalize | 每轮 baseline |
| 700 ms/write，queue 非空 stop | 74 encoded，35 written，queue 32/32，39 recording-only drops；stop 排空后完成 | baseline，VE/G2D 0/0 |
| writer EIO 接近 stop | writer failed，video 继续，finalize 报错 | baseline，VE/G2D 0/0 |
| video active 时完整 APP SIGINT | CAN、LapPro unavailable、logger、video 均 clean stop，rc=0 | baseline，VE/G2D 0/0 |
| 进程 SIGKILL | 用户态 cleanup 不运行；内核回收全部 DMA/ION | DMA/ION baseline；VE clock 留 1，见限制 |

没有发现 crash、deadlock、double free、use-after-free、DMA/ION 线性增长、
正常 stop 后的 fd/thread/clock 泄漏。encoder `VideoEncInit` 或
`AllocInputBuffer` 函数内部的精确中途失败没有动态注入；该 cleanup 由代码审计
确认始终有 native decoder VE owner 存活，但不能写成动态 PASS。

## 5 分钟完整 APP session

```text
fresh boot
→ 双 CAN + logger + LapPro unavailable/reconnect
→ 1280x720 G2D video record 5 min
→ SIGINT normal shutdown
```

| 时点 | dma-buf / ION | RSS | fd | thread | queue |
|---|---|---:|---:|---:|---|
| start 前 | 1 / 4,915,200 | — | — | — | 0 |
| 61 s | 10 / 40,591,360 | 15,308 KiB | 29 | 7 | 1, max 14 |
| 122 s | 10 / 40,591,360 | 15,564 KiB | 29 | 7 | 1, max 18 |
| 183 s | 10 / 40,591,360 | 16,296 KiB | 29 | 7 | 24, max 24 |
| 244 s | 10 / 40,591,360 | 15,964 KiB | 29 | 7 | 1, max 24 |
| video结束 | 10 / 40,591,360 | 10,236 KiB | 28 | 6 | 1, max 24 |
| APP stop 后 | **1 / 4,915,200** | process exited | 0 | 0 | 0 |

录像结果：8,373/8,373 written，0 application drop，duration 299.5053 秒，
PTS 单调，8,373/8,373 JPEG samples 有效，tail=`moov`，无 `.idx`。writer
queue 峰值 24 后恢复；writer max latency 878.165 ms。stop 后 VE/G2D
clock enable count 都回 0。

## Luna Max 独立复核

Luna Max 对生产 diff、精确板端 vendor ABI 和最终测试增量独立复核后结论为：

- `d003f8e` 建立了正确的双向 VE 生命周期窗口；
- partial init、recorder/encoder failure、writer drain 和 shutdown cleanup
  没有漏掉新的 P0 branch；
- worker join 与 JPEG queue copy ownership 保证 compositor pfree 不会 UAF；
- 未发现新增 data race、deadlock、double free 或 fd/dma cleanup 漏洞；
- 故障 preload 确实命中 ARM 产物的 `open/open64/dlopen`，不进入生产 target；
- encoder `dlopen` failure 不等同于 `VideoEncInit/AllocInputBuffer` 中途失败，
  最终报告必须保留该动态覆盖限制。

## 已解释的限制

1. `SIGKILL` 无法运行任何用户态 teardown。板端 kernel 能回收所有 dma-buf/ION，
   但 VE driver 日志为 `VE: release lost-lock...`，`ve/clk_enable_count` 留 1，需重启
   清除。这是 vendor kernel release path 限制，不是正常 video session stop；正常
   SIGINT 和全部 error/stop case 均回 0。本轮没有尝试用用户态代码“修复”不可捕获
   的 SIGKILL。
2. 全局拒绝 `/dev/ion` 会同时破坏 decoder、encoder 和 compositor 的 vendor
   allocator，不是单一 compositor allocation failure；该注入曾触发厂商
   `ion_alloc_close ref cnt error` 和 VE clock 残留，因此只记录诊断，不把它当成
   精确 PASS。四块 allocation 完成后的 G2D-open failure 已真实验证 partial
   compositor cleanup。
3. writer 永久阻塞仍会延迟 finalize 和 session stop；700 ms 长尾已证明最终能
   drain 并释放。这是既有 P1，不在本轮修改范围。

除上述已解释且不属于正常 session stop 的 vendor/kernel 限制外，没有未解释的
DMA/ION、fd、RSS、thread、queue 或 VE/G2D resource 线性增长。
