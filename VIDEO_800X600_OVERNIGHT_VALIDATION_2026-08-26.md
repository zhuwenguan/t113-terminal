# 800×600@30 T113 overnight validation — 2026-08-26/27

## 2026-08-27 product-requirement clarification

The product owner confirmed that one hour is the required maximum duration.
For the supported `--video-seconds <= 3600` scope, the product verdict is:

- **800×600@30:** `PASS WITH CONDITIONS`
- **Suitable as the formal default recording format:** `YES`
- **Video performance work:** `FREEZE VIDEO PERFORMANCE WORK`

The condition is now enforced by commit `c5e18b4`: the CLI rejects values
above 3,600 before video or hardware initialization.  Host tests (13/13), an
ARM build, board-side rejection of 3,601, and a normal five-second board
recording all passed.  The normal smoke produced 99 application/MP4 frames,
monotonic PTS, no residual `.idx`, and exact clock/DMA/ION baseline recovery.

The original two-hour stress verdict below remains part of the audit record.
It proves that the old build silently truncated unsupported requests; it is no
longer a supported product scenario after the explicit CLI rejection.

## Original two-hour stress verdict

- **G2D clock P0:** `G2D CLOCK P0 VERIFIED`
- **DMA/ION teardown:** `VERIFIED`
- **800×600@30:** `FAIL`
- **Suitable as the formal default recording format:** `NO`
- **Video performance work:** `FURTHER WORK REQUIRED`

The G2D clock and DMA teardown fixes survived all completed lifecycle and error
tests.  The overall configuration nevertheless fails acceptance because a
requested two-hour recording silently stopped accepting frames after exactly
108,000 frames (the built-in one-hour-at-30-FPS limit).  The application kept
running for the second hour and reported success, while 106,764 subsequent
frames were counted as application drops.  This is a new P0 data-loss defect.

The post-long-session 50-round phase and optional four-hour session were not
run.  Continuing them would violate the fail-fast rule and would not add useful
evidence until the duration-limit P0 is fixed.

## A. Tested build and environment

| Item | Value |
|---|---|
| Test-helper HEAD | `50da872` (`test(video): harden overnight pass criteria`) |
| Business/G2D-fix documentation base | `85fddb9` |
| Application binary | `/mnt/UDISK/t113_terminal_800x600_772ec72` |
| Application SHA-256 | `f22fabb1b8caf126a2b6c20c0b935403ca01921bcc3596de92305c91d1204d79` |
| Kernel | `Linux kickpi 5.4.61 #1 SMP PREEMPT Mon Jun 29 10:39:34 CST 2026 armv7l` |
| Boot ID | `f817b244-f3e9-4e97-a553-828cb1e2b7e6` |
| Camera mode | Native UVC `MJPEG 800×600 @ 30 FPS`, 3 V4L2 buffers |
| Encoder | Cedar hardware MJPEG, ARGB8888 input, quality 82 |
| Test root | `/mnt/usb/sda/video_overnight_20260826_50da872` |
| Formal test start | 2026-08-26 20:35 HKT |
| Two-hour phase | 2026-08-26 22:22 to 2026-08-27 00:27 HKT |
| Reboot during formal run | No; boot ID checked after every session |
| Business-code changes during run | None |

The camera advertised native `MJPG 800×600` with a discrete 0.033-second
interval (30 FPS).  No 720P capture-and-resize fallback was used.

Fresh-boot baseline:

| Resource | Baseline |
|---|---:|
| G2D enable/prepare | `0/0` |
| bus-g2d enable/prepare | `0/0` |
| mbus-g2d enable/prepare | `0/0` |
| dma-buf | 1 object / 4,915,200 B |
| ION system heap | 1 buffer / 4,915,200 B |
| ION reserved heap | 0 / 0 B |
| MemAvailable | 66,856 KiB |
| System threads | 83 |
| Temperature | 45.694°C |
| CPU governor/frequency | performance / 1,008,000 kHz |

## B. G2D clock P0 regression

`G2D CLOCK P0 VERIFIED`

The completed formal run included 100 regular lifecycle sessions, 32
mixed-duration sessions, 50 rapid start/stop sessions, three benchmarks, five
writer-stall sessions, seven effective writer-error sessions, and the failed
two-hour correctness session.  Every completed teardown restored all three
clock domains to the same fresh-boot `enable=0, prepare=0` baseline.

The two-hour session kept all three clock domains at `1/1` while active and
returned all of them to `0/0` after stop.  There was no prepare-count growth,
single unexplained residual, or enable/prepare mismatch.

Previously observed bad state versus this run:

| Point | G2D prepare after stop | FPS | YUV→ARGB avg | Blend avg | Video CPU |
|---|---:|---:|---:|---:|---:|
| Before fix abnormal state | 5 | 24.66 | 7.466 ms | 20.371 ms | 83.3% |
| Lifecycle round 1 | 0 | 29.99 | 3.058 ms | 4.911 ms | 42.3% |
| Lifecycle round 50 | 0 | 29.99 | 3.093 ms | 4.855 ms | 42.1% |
| Lifecycle round 100 | 0 | 30.00 | 3.050 ms | 4.903 ms | 42.2% |
| Two-hour session around 30 min | active 1, later stop 0 | 29.94 | 3.22 ms | 14.33 ms | n/a at checkpoint |
| Two-hour session around 60 min | active 1, later stop 0 | 29.93 | 3.27 ms | 14.33 ms | n/a at checkpoint |

The clock-reference P0 is fixed, but the G2D blend slow state remains a
separate performance P1: after many sessions, blend latency rose from about
4.9 ms to about 14–22 ms without any clock-reference leak.  It did not trend
upward further during the long session.

## C. DMA/ION teardown regression

`VERIFIED`

Every accepted session returned to 1 dma-buf / 4,915,200 B and 1 ION system
buffer / 4,915,200 B.  The reserved ION heap stayed at zero.  The two-hour
session used 10 buffers / 29,491,200 B while active and returned to the exact
fresh baseline after stop.  The former `+12,443,648 B/session` linear growth
did not recur.

## D. 100-round lifecycle test

All 100 sessions passed the strict per-session gates:

- negotiated camera mode matched native MJPEG 800×600@30;
- both CAN receiver paths observed frames;
- application exit was bounded and clean;
- application `written` equalled MP4 sample count;
- PTS was monotonic, `moov` was present, JPEG markers were complete, and no
  `.idx` remained;
- boot ID, kernel and binary SHA remained unchanged;
- clocks and DMA/ION returned to the exact baseline.

| Metric | Round 1 | 10 | 25 | 50 | 75 | 100 |
|---|---:|---:|---:|---:|---:|---:|
| FPS | 29.99 | 30.01 | 30.00 | 29.99 | 30.01 | 30.00 |
| YUV→ARGB avg | 3.058 | 3.109 | 3.065 | 3.093 | 3.041 | 3.050 ms |
| YUV→ARGB P99 | 4.400 | 4.400 | 3.800 | 4.800 | 3.800 | 3.800 ms |
| Blend avg | 4.911 | 4.923 | 4.944 | 4.855 | 4.894 | 4.903 ms |
| Blend P99 | 7.000 | 7.300 | 7.300 | 6.300 | 6.200 | 6.300 ms |
| Encode avg | 2.619 | 2.626 | 2.602 | 2.595 | 2.589 | 2.623 ms |
| Video CPU | 42.3% | 42.5% | 41.9% | 42.1% | 41.8% | 42.2% |
| Queue peak | 1 | 1 | 1 | 1 | 1 | 1 |
| DMA after stop | 4,915,200 B | same | same | same | same | same |
| G2D prepare after stop | 0 | 0 | 0 | 0 | 0 | 0 |

Across all 100 rounds: FPS 29.97–30.02, application drop 0, queue full 0,
and no systematic latency increase.

## E. Mixed and rapid lifecycle

The mixed test ran 32 shuffled sessions covering 1 s, 2 s, 5 s, 10 s, 30 s,
60 s, 3 min and 5 min.  All lifecycle/file/resource gates passed.  Eleven
sessions exhibited the intermittent G2D slow state; the worst session was
22.16 FPS with YUV→ARGB about 10.8 ms and blend about 21.6 ms.  This was
retained as a performance P1 and did not hide a resource leak.

The rapid test ran 50 start/stop cycles with 200, 300, 400 and 500 ms delays.
All passed.  Zero-frame immediate-stop sessions were accepted only when no
frame had been written; any non-empty MP4 still had to pass frame/sample/PTS
checks.  No crash, deadlock, UAF, double free, stale writer data, clock
residual, DMA leak, or orphan application process was observed.

## F. 800×600 short performance

| Metric | 10 s | 60 s | 5 min |
|---|---:|---:|---:|
| Capture/encode/write FPS | 30.00 | 30.00 | 29.89 |
| Frames written | 286 | 1,786 | 8,954 |
| Camera/driver sequence loss | 1 | 1 | 33 |
| Application drop | 0 | 0 | 0 |
| Decode avg / P95 / P99 / max | 3.838 / 3.900 / 5.200 / 13.511 ms | 3.858 / 3.900 / 4.200 / 12.089 ms | 3.830 / 3.900 / 4.000 / 14.345 ms |
| YUV→ARGB avg / P95 / P99 / max | 3.370 / 3.900 / 4.800 / 7.476 ms | 3.293 / 3.800 / 4.700 / 8.042 ms | 3.251 / 3.500 / 4.200 / 11.245 ms |
| Blend avg / P95 / P99 / max | 5.541 / 6.600 / 7.400 / 8.252 ms | 5.471 / 6.400 / 7.400 / 12.145 ms | 5.331 / 5.900 / 6.700 / 10.381 ms |
| Encode avg / P95 / P99 / max | 2.629 / 2.800 / 4.100 / 4.231 ms | 2.586 / 2.700 / 3.100 / 4.435 ms | 2.578 / 2.600 / 2.900 / 5.723 ms |
| Writer avg / P95 / P99 / max | 1.383 / 1 / 27 / 54.702 ms | 1.566 / 1 / 24 / 121.919 ms | 1.899 / 1 / 28 / 829.549 ms |
| Queue avg / peak / full | 1.00 / 2 / 0 | 1.01 / 3 / 0 | 1.09 / 19 / 0 |
| Video CPU | 44.0% | 42.6% | 39.1% |
| Max RSS | 9,636 | 9,692 | 10,712 KiB |

MP4 frame intervals:

| Session | Avg | P50 | P95 | P99 | Max |
|---|---:|---:|---:|---:|---:|
| 10 s | 33.335 | 32.033 | 36.033 | 36.122 | 36.322 ms |
| 60 s | 33.336 | 32.033 | 36.011 | 36.111 | 81.833 ms |
| 5 min | 33.455 | 32.022 | 35.989 | 36.100 | 843.933 ms |

The fixed production logger does not emit per-stage P50 values; exact stage
P50 and input-MJPEG average byte size therefore remain unavailable.  They are
not fabricated in this report.

## G. Long-duration result and new P0

The requested two-hour phase failed its duration gate:

| Item | Expected | Actual |
|---|---:|---:|
| Requested run | 7,200 s | pipeline process ran about 7,378 s including harness overhead |
| Recorded duration | approximately 7,200 s | **3,624.916133 s** |
| Expected frames at actual input rate | approximately 215k | **108,000** |
| Application written / MP4 samples | equal | 108,000 / 108,000 |
| Application drops | approximately 0 | **106,764** |
| Recorder failure flag | failure or explicit stop reason if capped | `recording_failed=no`, `writer_failed=no` |
| File structure | valid | valid `moov`, monotonic PTS, 108,000/108,000 JPEG markers |

The first line with the exhausted limit appeared at application time
3,626.562 s: encoded/written remained 108,000 while `app_drop` began growing.
At 7,199.999 s, capture was still 30.03 FPS but encode/enqueue/write were all
0.00 FPS and `app_drop` had reached 106,758.

### Root cause

- `include/video_recorder.h:16` defines
  `VIDEO_RECORDER_MAX_DURATION_SECONDS` as `60U * 60U`.
- `src/video_recorder.c:855`–`870`, `video_recorder_begin_frame()`, only
  accepts frames while `producer_frame_index < 3600 * fps`.  At 30 FPS this is
  exactly 108,000 frames.
- On reaching the limit it merely increments `dropped_frames`; it does not set
  inactive/failed state or report a stop reason.
- `video_recorder_is_recording()` therefore continues returning true, and the
  pipeline counts every rejected frame as an application drop.
- Independently, `src/video_pipeline.c:802`–`805` correctly keeps the video
  pipeline alive for the requested 7,200 monotonic seconds.

This is not a test-harness duration error.  The MP4 frame count, application
counters, source constant, and exact transition all agree.

### Severity and minimum safe correction

Severity: **P0 — silent loss of the second hour of requested video while the
application reports a successful recorder and exits with status 0.**

Minimum safe correction for a subsequent task:

1. make the supported recorder duration explicit and at least as large as the
   configured session duration (2 h is the immediate requirement);
2. reject unsupported requested durations during initialization instead of
   silently accepting them;
3. if a runtime frame/sample limit is reached, set a visible recorder failure
   or explicit completion reason rather than counting all later frames as
   ordinary drops;
4. add a boundary test with a deliberately small configured limit and a T113
   two-hour regression requiring duration near 7,200 s and approximately 215k
   samples.

No correction was made during this fixed-binary validation run.

### Luna Max independent review

Luna Max independently reviewed the final evidence and source without changing
files.  Its conclusion matches the primary analysis:

- the duration gate is a true failure, not a harness false positive;
- `108000 == 3600 * 30` identifies the recorder frame cap exactly;
- the CLI accepts 7,200 seconds while the recorder silently supports only one
  hour;
- this is not an integer overflow and the valid MP4 structure does not negate
  the missing second hour;
- severity is P0, and `postlong50`/`long4h` must not continue.

Luna also noted the harness's wall-clock sampling overhead, which delayed the
outer APP stop by several minutes.  That can only lengthen the process run; it
cannot explain an MP4 ending about one hour early.

## H. File integrity

All successful short, mixed and benchmark sessions passed:

- application written count equals MP4 sample count;
- PTS monotonic and no duplicate/backward timestamps reported;
- `moov` and normal file tail present;
- JPEG SOI/EOI markers present for every sample inspected by the tool;
- no residual `.idx` after successful finalize.

The two-hour-request file is structurally valid for its first 3,624.916 s, but
fails correctness because the second requested hour is absent.  A structurally
valid truncated-duration result is still a failure.

The board has no full-frame ffmpeg decode validation in this run.  The current
inspector validates every sample's JPEG markers and inspects first/middle/last
sample offsets, but does not independently decode random HUD frames.  That
coverage is explicitly incomplete.

## I. Writer stall and failure injection

The stdio-level preload injected five periodic stalls for each delay and then
allowed recovery:

| Requested stall | Observed writer max | Queue peak | Full/drop | End depth | Result |
|---:|---:|---:|---:|---:|---|
| 100 ms | 102.388 ms | 4 | 0/0 | 0 | PASS |
| 300 ms | 707.803 ms (includes storage tail) | 15 | 0/0 | 0 | PASS |
| 500 ms | 502.529 ms | 16 | 0/0 | 0 | PASS |
| 700 ms | 701.603 ms | 22 | 0/0 | 0 | PASS |
| 1,000 ms | 1,001.532 ms | 31 | 0/0 | 0 | PASS |

In every case capture/encode continued, the queue recovered to 0/1, all
encoded frames were written, and teardown returned to clock/DMA baseline.

Effective error cases: short write, EIO, ENOSPC, EROFS/read-only media,
finalize `fflush` EIO, near-stop EIO, and EIO after a 1,000 ms delay with a
30-frame queue backlog.  All produced application exit 0 while setting writer
failure; CAN and logger continued, and resources were released.  For the
queue-backlog EIO, capture continued at 30 FPS, queue peak was 30, and logger
wrote all 13,803 queued records with no logger errors.

These are synthetic `fwrite`/`fflush` failures and delays, not a physical USB
device removal.  That limitation is retained.

## J. Resource stability

During the long session:

| Metric | Observed |
|---|---:|
| RSS active range / max | approximately 9–10.8 MiB / 10,776 KiB |
| Application fd / thread maximum | 27 / 7 |
| Active DMA/ION | 29,491,200 B / 10 buffers |
| Post-stop DMA/ION | 4,915,200 B / 1 buffer |
| Post-stop G2D clocks | all `0/0` |
| CPU frequency | fixed at 1,008,000 kHz |
| Total system CPU over long run | 70.0% |
| Final cumulative video-process CPU | 119.1% across cores |
| MemAvailable range | 25,588–65,668 KiB |

No RSS, fd, thread, DMA, ION or clock-reference linear leak was observed.

The synthetic CAN load did not survive the long phase: CAN1 entered bus-off at
14.073 s and CAN0 at 22.366 s.  The application correctly isolated both local
failures and continued video/HUD/LapPro-failure handling, but this means the
two-hour phase did **not** maintain the requested full CAN load.  The long-run
full-load coverage is therefore incomplete and must be repeated with a stable
CAN stimulus after the recorder P0 is fixed.

LapPro hardware remained unavailable as required.  There were 174 failed
connections in about 7,378 s (roughly one per 42 s), with no busy loop, crash,
or shutdown leak.  The normal LapPro data path is **NOT VERIFIED**.

## K. Thermal behaviour

Temperature ranged from 48.374°C to 53.935°C.  CPU frequency stayed at
1,008,000 kHz with the performance governor.  No thermal throttling was
observed.  Around 30 and 60 minutes, capture remained about 29.9–30.0 FPS and
G2D latency was stable near 3.2 ms YUV→ARGB and 14.3 ms blend.

For the portion where recording was active, one-second encode FPS statistics
were: average 29.782, P1 24.75, P5 28.20, median 29.99, P95 30.14, and a
longest run of three consecutive windows below 29 FPS.  MP4 average output was
29.793793 FPS.  Thus the first recorded hour was close to input on average but
did not demonstrate the requested comfortable performance margin.

## Skipped after fail-fast

- 50 short sessions after the long recording: not run because the two-hour
  duration P0 had already failed.  The long-session teardown itself did return
  resources to baseline.
- Optional 3–4 hour session: not run; the current recorder cannot exceed
  108,000 frames.
- Normal LapPro hardware path: not run because the device cannot power on.

## Required next action

The single blocking correctness issue is the recorder's silent one-hour frame
limit.  Fix and explicitly test that P0 first, then rerun a fresh-boot two-hour
session with stable CAN stimulus.  Only after a full-duration file contains
approximately all 215k input frames should the post-long 50-round reuse test
and any optional longer session resume.

After that correctness gate, the only demonstrated compute-performance issue
is the intermittent G2D blend slow state (about 14–22 ms versus about 4.9 ms in
fresh short sessions).  It should be investigated as a focused P1; no broad
pipeline rewrite or speculative optimization is justified by this run.
