# T113 HUD TTF OOM 排查记录

日期：2026-08-28

## 结论

本次 OOM 没有证据表明是 TTF 字体模块或 glyph cache 的内存泄漏。更符合下面的原因：

```text
T113 可用内存约 100 MB、无 swap
        ↓
系统已有较大的 Shmem/缓存和显示 DMA-BUF
        ↓
启动摄像头、解码器、G2D、编码器时出现一次性多媒体缓冲峰值
        ↓
低内存水位触发内核 OOM killer
```

FreeType face 是进程生命周期级资源，glyph cache 是固定容量的运行时缓存；重启后的不录制和录制测试都显示 RSS 在启动分配完成后保持平稳，停止进程后 ION/DMA-BUF 回到显示基线。

## OOM 发生前的板端证据

重启前板端状态：

- `MemTotal = 102312 kB`，无 swap。
- `MemAvailable` 约 `21.2 MB`，`MemFree` 约 `17.5 MB`。
- `Shmem` 约 `38.7 MB`。
- 显示 ION/DMA-BUF 已常驻约 `4.9 MB`，1 个 buffer。
- 当时没有残留的 `race_video`/`t113_terminal` 主进程。

内核 OOM 记录中，被杀进程本身的 RSS 很小：

```text
t113_terminal: total-vm 96,396 kB, anon RSS 8,468 kB, file RSS 2,756 kB
race_video:    total-vm 45,828 kB, anon RSS 6,432 kB
```

这说明 OOM 是“申请新的系统/多媒体内存时整体可用水位不足”，不是应用 heap 已经随帧无限增长。RSS 数字也不支持“字体缓存吃满内存”的判断。

## 重启后的复测

开发板已实际重启，启动后先记录空闲基线，再按不录制和录制两条真实路径测试。

| 场景 | RSS | ION/DMA-BUF | 结果 |
| --- | ---: | ---: | --- |
| 干净空闲基线 | — | 4.9 MB / 1 buffer | `MemAvailable` 约 67.1 MB，无 OOM |
| `race_video` 不录制，持续 69 秒以上 | `0.9 → 7.7 → 11.9 MB`，随后稳定 | `4.9 → 19.2 MB`，随后稳定 | 摄像头、G2D HUD 正常，约 30 FPS，无 OOM |
| 完整录制，约 10 秒 | `12.2–12.4 MB` 稳定 | `29.5 MB / 10 buffers`，随后稳定 | 285 帧全部处理/编码/写入，0 drop，无 OOM |
| 停止录制进程后 | 进程退出 | 回落至 4.9 MB / 1 buffer | 编解码/G2D 缓冲已释放，无 OOM |

完整录制日志：

```text
camera/decoded/processed/encoded/enqueued/written = 285/285/285/285/285/285
capture/decode/process/encode/write FPS            ≈ 30.00
app_drop = 0, writer dropped = 0, writer full = 0
HUD total average / P95 / P99                     = 2.333 / 3.800 / 4.600 ms
G2D healthy = yes, recording_failed = no
```

生成的 MP4 经过检查：285 帧、9.501433 秒、PTS 单调、285/285 JPEG marker、`moov` 在文件尾部，说明录制链路没有因内存压力产生隐性损坏。

## 为什么这不是按帧泄漏

不录制路径的 RSS 只在摄像头/解码缓冲初始化时从约 `0.9 MB` 上升至 `11.9 MB`，之后持续 60 秒以上没有单调增加；ION/DMA-BUF 也只从显示基线增加到固定的 8 个对象。录制路径同样在编码器缓冲建立后固定在约 `29.5 MB`，而停止进程后回落到 `4.9 MB`。

这正是缓冲池初始化的形态，而不是下面这种泄漏形态：

```text
每帧处理 → RSS/ION 持续上升 → 长时间运行后无法回落
```

本次 TTF HUD 的常驻资源包括 FreeType face、固定容量 glyph cache 和 PNG HUD 资源。ARM benchmark 中 HUD 的 `resident_bytes` 约 `5,493,813` 字节；它是启动期固定占用，不随帧数增长，也不足以解释 OOM 前的系统低内存状态。

## 后续建议

当前不需要为了这次 OOM 回退到点阵字体，也没有必要修改 glyph cache。产品启动时应保留足够内存余量，并避免在同一低内存阶段叠加多个视频进程或重复初始化多媒体设备；如果后续需要进一步降低启动峰值，应优先审查 Cedar/V4L2/G2D buffer 数量、CMA/ION 预留和服务启动顺序。

本次排查产生的 TTF/OOM 测试资料已迁移到 U 盘：`/mnt/usb/sda/t113_hud_ttf_validation_20260828/`；板端 `/mnt/UDISK` 中对应的 3 个临时测试目录和 `/tmp/t113-oom-investigate-06ZHIu` 已清理，没有修改应用源码。
