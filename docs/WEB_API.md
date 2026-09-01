# Web API

Phase 1 is a local, read-only API served by the independent `webd` process.
The server reads one latest `VehicleState` cache populated from the
`/race_vehicle_state` POSIX shared-memory object. It does not send commands to
the vehicle and does not expose video yet.

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
  "websocket_clients": 1
}
```

When no publication has ever been observed, `vehicle_state_age_ms` is `null`.
The age is based on `CLOCK_MONOTONIC`, and `vehicle_state_alive` becomes false
after one second without a fresh core publication.

`GET /api/capabilities` currently returns:

```json
{"mode":"local","telemetry":true,"video":false,"logs":false,
 "config":false,"remote_video":false}
```

`WS /ws/telemetry` sends the same `/api/state` JSON schema as text frames. The
default sampling limit is 20 Hz, configurable with `telemetry_rate_hz`; a
state is sent only when the shared-state sequence changes, so a slower core
does not receive fabricated updates. The cache is read once per tick, and a
WebSocket sender only consumes the cached JSON.

There is no `/video/live.mjpg` implementation in Phase 1. The route remains
reserved by the `video=false` capability response.
