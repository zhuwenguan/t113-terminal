# webd

`webd` is the local, read-only HTTP/WebSocket daemon for Phase 1. It maps the
authoritative `/race_vehicle_state` and independent `/race_bms_state` POSIX
shared-memory objects read-only via `race_ipc`, reads the latest original
MJPEG from `/race_latest_jpeg`, serves `web/dashboard/dist`, and exposes
`/api/state`, `/api/system`, `/api/capabilities`, `/api/bms`,
`/ws/telemetry`, and `/video/live.mjpg`.

The HTTP and WebSocket protocol implementation is the vendored CivetWeb 1.16
library under `third_party/civetweb/` (MIT license). CivetWeb is built without
TLS, CGI, Lua, compression, or HTTP/2 because those features are outside this
local Phase 1 boundary.

Host smoke run:

```sh
cmake --preset host-debug
cmake --build --preset host-debug --target webd fake_state_producer
out/host-debug/fake_state_producer --name /t113_webd_demo --rate-hz 20 &
out/host-debug/webd --port 18080 --state-shm-name /t113_webd_demo \
  --web-root web/dashboard/dist
```

For a host browser preview, start the test-only original-JPEG producer with a
separate object name and point `webd` at it:

```sh
cmake --build --preset host-debug --target fake_video_producer
out/host-debug/fake_video_producer --name /t113_web_video_demo \
  --source-fps 30 --publish-fps 15 &
out/host-debug/webd --port 18080 --state-shm-name /t113_webd_demo \
  --video-shm-name /t113_web_video_demo --web-root web/dashboard/dist
```

Open `http://127.0.0.1:18080/`. The dashboard uses a native `<img>` MJPEG
stream and shows a stale/offline placeholder when the producer stops. The
producer is test-only; production `race_video` publishes the camera's original
V4L2 MJPEG bytes without web-side decode or re-encode. Video allows four
clients and has no per-client unbounded frame queue.

Open `http://127.0.0.1:18080/bms.html` for the BMS Info page. BMS values are
read from the separate `/race_bms_state` snapshot and refreshed once per
second. The page shows KPE601 HV1--HV5/current, all 96 cell values, the eight
real-time temperatures per KPL07 board, board/fault status, and a bounded
scrollable edge-detected fault log.

The producer is only a host test tool; it is not part of the T113 runtime.
The installed board layout uses `<prefix>/web` and the sample board config
expects `/opt/t113/web` when installed with `--prefix /opt/t113`.
