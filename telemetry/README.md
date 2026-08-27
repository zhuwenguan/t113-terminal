# race_telemetry skeleton

Future telemetry reads shared state, downsamples it, and owns reconnect/backoff
and all blocking network operations. MQTT/TCP/4G are intentionally absent.
