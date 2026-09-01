# webd

`webd` is the local, read-only HTTP/WebSocket daemon for Phase 1. It maps the
authoritative `/race_vehicle_state` POSIX shared-memory object read-only via
`race_ipc`, serves `web/dashboard/dist`, and exposes the three JSON endpoints
plus `/ws/telemetry`.

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

The producer is only a host test tool; it is not part of the T113 runtime.
The installed board layout uses `<prefix>/web` and the sample board config
expects `/opt/t113/web` when installed with `--prefix /opt/t113`.
