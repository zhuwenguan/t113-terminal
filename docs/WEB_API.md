# Web API

Phase 1 is a local, read-only API served by the independent `webd` process.
The server reads one latest `VehicleState` cache populated from the
`/race_vehicle_state` POSIX shared-memory object and one latest original MJPEG
frame cache populated from `/race_latest_jpeg`. It does not send commands to
the vehicle.

BMS data is a separate passive-monitor snapshot populated by `race_core` from
`awlink1` and read from `/race_bms_state`. It is not part of `VehicleState` or
the vehicle telemetry WebSocket.

## Endpoints

`GET /api/state` returns `200` with the latest fresh state. If the core has not
published a state or the last publication is older than one second it returns
`503` and `{"valid":false,"error":"vehicle_state_unavailable"}`.

The successful response is explicit JSON rather than a C struct dump:

```json
{
  "valid": true,
  "source_sequence": 42,
  "timestamp_ms": 123456,
  "timestamp_us": 123456789,
  "valid_mask": 0,
  "can_valid": true,
  "gps_valid": true,
  "speed_kph": 0.0,
  "heading_deg": 0.0,
  "latitude_deg": 0.0,
  "longitude_deg": 0.0,
  "acc_x_mps2": 0.0,
  "acc_y_mps2": 0.0,
  "acc_z_mps2": 0.0,
  "motor_rpm": 0.0,
  "torque_nm": 0.0,
  "motor_temperature_c": 0.0,
  "controller_temperature_c": 0.0,
  "battery_temperature_c": 0.0,
  "hv_voltage_v": 0.0,
  "hv_current_a": 0.0,
  "hv_power_w": 0.0,
  "hv_power_kw": 0.0,
  "hv_soc_percent": 0.0,
  "hv_discharge_ah": 0.0,
  "hv_software_discharge_ah": 0.0,
  "lv_voltage_v": 0.0,
  "throttle_percent": 0.0,
  "brake_percent": 0.0,
  "mode": 0,
  "drive_state": "IDLE",
  "event_mode": "UNKNOWN",
  "lap_number": 0,
  "lap_valid": false,
  "lap_time_s": 0.0,
  "last_lap_time_s": 0.0,
  "best_lap_time_s": 0.0,
  "delta_s": 0.0,
  "track_distance_m": 0.0,
  "sector_number": 0,
  "accel_distance_m": 0.0,
  "accel_50m_time_s": 0.0,
  "accel_time_s": 0.0,
  "max_acceleration_mps2": 0.0
}
```

`speed_kph` is the API spelling for the existing `VehicleState.speed_kmh`
field. `hv_power_kw` is derived from the existing `hv_power_w` field; no new
vehicle signal is introduced.

`GET /api/system` returns service and source health:

```json
{
  "uptime_s": 12,
  "vehicle_state_alive": true,
  "vehicle_state_age_ms": 18,
  "vehicle_state_sequence": 42,
  "websocket_clients": 1,
  "video_clients": 1,
  "video": {
    "available": true,
    "status": "live",
    "width": 800,
    "height": 600,
    "source_fps": 30.0,
    "web_publish_fps": 15.0,
    "frame_age_ms": 35,
    "jpeg_size_bytes": 120000,
    "dropped_by_rate_limit": 42
  }
}
```

When no publication has ever been observed, `vehicle_state_age_ms` is `null`.
The age is based on `CLOCK_MONOTONIC`, and `vehicle_state_alive` becomes false
after one second without a fresh core publication.

`GET /api/capabilities` currently returns:

```json
{"mode":"local","telemetry":true,"video":true,"bms":true,
 "logs":false,"config":false,"remote_video":false}
```

`GET /api/bms` returns the latest BMS snapshot. It contains `meter`, four
`boards`, and a newest-first fixed-capacity `fault_events` array. `board.id`
is zero-based and `board.board_number` is one-based for display. `cells_v`
always has 24 positions and `temps_c` always has exactly the eight live
temperature positions T01--T08; their companion validity arrays identify
which positions are current. Stale measurement values are JSON `null`, never
fabricated zeroes. Fault arrays retain T01--T24 and balance channels even when
the corresponding live measurement tile is not displayed. Fault events are
created only on a 0->1 or 1->0 transition and contain `FAULT` or `RECOVERED`.

The BMS response uses `status` values `LIVE`, `WAITING`, or `STALE`. If the
shared BMS object has never been published, the endpoint returns HTTP 503 with
`status: "OFFLINE"`. The API is polled by `/bms.html` once per second so the
existing vehicle-state WebSocket remains unchanged.

`GET /video/live.mjpg` is a native browser-compatible
`multipart/x-mixed-replace` MJPEG stream. The dashboard attaches one `<img>`
to this route after `/api/system.video.status` becomes `live`; it does not
fetch or decode individual frames in JavaScript. The stream uses the
latest-frame cache, has no per-client unbounded queue, allows at most four
simultaneous video clients, and closes when the source is stale for one second
or a write exceeds the server's three-second request timeout. A fifth client
receives HTTP 503 with `video_client_limit`; no source or recorder frame is
dropped because of a web client.

`WS /ws/telemetry` sends the same `/api/state` JSON schema as text frames. The
default sampling limit is 20 Hz, configurable with `telemetry_rate_hz`; a
state is sent only when the shared-state sequence changes, so a slower core
does not receive fabricated updates. The cache is read once per tick, and a
WebSocket sender only consumes the cached JSON.

The producer is the existing `race_video` V4L2 loop: after each DQBUF it copies
the camera's original MJPEG once into a two-slot, seqlock-protected POSIX
shared-memory object, before the existing decode/HUD/record path and without a
second camera capture. Publication is PTS-based and limited to 15 FPS by
default. `webd` copies only the newest complete frame into a bounded cache;
slow browsers cannot back-pressure the camera or recorder.
