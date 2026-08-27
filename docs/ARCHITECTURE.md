# T113 race daemon architecture

## Phase 1 outcome

The runtime fault boundary is `race_core + POSIX shared memory + race_video`.
The legacy `t113_terminal` target remains available for A/B comparison and
rollback. No CAN mapping, Lap/Delta/Accel algorithm, HUD drawing, encoder,
container or 800x600 pipeline behavior was redesigned.

```text
CAN -----\
          +--> race_core --> /race_vehicle_state --> race_video
LapPro --/          |                    |----------> future logger/web/telemetry
                    +--> logger (phase 1)
```

Processes are fault domains; threads remain the concurrency mechanism inside a
process.

## Responsibilities

- `race_core`: dual SocketCAN RX/parser, LapPro Bluetooth/GPS/IMU, authoritative
  VehicleState, lap/delta/accel, timestamps, race events, HMI and the currently
  validated logger. It rejects video command-line options.
- `race_video`: V4L2 capture, JPEG/MJPEG decode, G2D, HUD, compositor, JPEG
  encode, MP4 writer, frame queues and existing DMA/ION teardown.
- `race_logger`: reserved storage fault domain. It is a disabled-by-default
  skeleton because raw CAN/LapPro/state batching and session paths remain
  tightly coupled to core.
- `race_web` and `race_telemetry`: disabled-by-default skeletons. They contain no
  HTTP, WebSocket, MQTT, TCP or 4G implementation.

Source modules remain in `src/` during phase 1 to reduce path-only churn. The
`core/`, `video/`, `logger/`, `web/` and `telemetry/` directories state process
ownership; physical source relocation can follow after board validation.

## Shared VehicleState ABI

`common/include/race/vehicle_state.h` is the only authoritative definition.
The old include is a compatibility forwarding header. `librace_common.a`
provides SHM and monotonic-clock utilities.

`RaceSharedState` contains magic `0x52414345`, ABI version, header size, total
size, VehicleState size, an atomic sequence, flags, `publish_mono_us`, and the
state. The object name is `/race_vehicle_state`. Core is the single writer; all
other processes map it read-only. A reader rejects bad magic/version/sizes with
`EPROTO`.

Consistency uses a 32-bit lock-free C11 atomic seqlock. Core marks `seq` odd,
executes a full fence, copies state/timestamp, then release-stores an even
sequence. Readers acquire-load the first sequence, reject odd values, copy,
execute a full fence, and accept only an unchanged even sequence. This avoids
torn snapshots on weakly ordered ARM; it does not rely on `volatile`.

All internal timestamps use `CLOCK_MONOTONIC` microseconds. Readers treat a
publish age over one second (or a clock inversion/zero timestamp) as stale.
`race_video` then supplies a zero/default state to HUD and logs transitions
between `CORE STATE STALE` and `CORE STATE LIVE`.

## Lifecycle and failure behavior

Both daemons handle SIGINT/SIGTERM by setting a signal-safe stop flag. Core's
CAN epoll loop exits, then joins the snapshotter and LapPro, stops/flushed the
logger, closes HMI/resources and unmaps SHM. Video stops its pipeline, whose
existing teardown joins queues/writer and releases recorder, encoder, G2D,
DMA/ION and V4L2 resources before unmapping SHM.

Killing `race_video` cannot terminate core because there is no process ownership
relationship or writable core mapping. Killing core leaves the already mapped
SHM valid; video detects the frozen publish timestamp. Restarting video opens
camera/G2D/encoder again. Restarting core reinitializes the ABI in the same SHM
object.

## Build, install and operation

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-t113.cmake
cmake --build build -j
cmake --install build --prefix /opt/race
scripts/start_race.sh
scripts/stop_race.sh
```

`RACE_BIN_DIR`, `RACE_RUN_DIR` and `RACE_LOG_DIR` preserve existing deployment
flexibility. Start launches core, waits for `/dev/shm/race_vehicle_state`, then
launches video. Stop sends SIGTERM in telemetry/web/video/logger/core order and
waits up to ten seconds per process. Restart video alone with:

```sh
kill -TERM "$(cat /run/race/race_video.pid)"
/opt/race/bin/race_video &
```

Use `race_state_dump` to inspect sequence, monotonic age, freshness and key HUD
fields. Future web/telemetry/logger readers must use the same read-only API.
Events/storage payload transport is intentionally only reserved in phase 1.

## Required T113 acceptance

Host tests prove ABI validation, consistent reads, freshness logic and existing
algorithm/video unit tests. They cannot certify device drivers or hardware.
Before replacing the legacy binary, run on a real T113:

1. Confirm separate PIDs with `ps`/`top`; compare CAN, LapPro and dump fields.
2. Record 800x600 video and verify camera, HUD, G2D, encode, MP4, PTS and FPS.
3. `killall race_video`; verify core/CAN/LapPro/logger continue, then restart it.
4. Stop core; verify video remains alive and reports stale/default HUD state.
5. Repeat video start/stop at least ten times while measuring fd, memory,
   dma-buf, ION, G2D, camera and encoder resources.
6. Run an overnight comparison against the recorded legacy baseline and check
   CPU, RSS growth, sequence progress, drops and latency percentiles.

Do not delete `t113_terminal` until these checks pass. Phase 2 should define a
bounded event/log IPC and only then move storage writes into `race_logger`;
phase 3 may add read-only web and downsampled telemetry consumers.
