# T113 deployment

The Phase 1 install includes `race_core`, `webd`, `race_video`, the sample
`config/webd.conf`, the static dashboard under `web/`, and BusyBox SysV startup
scripts. `race_video` publishes
the original camera MJPEG through `/race_latest_jpeg`; `webd` serves it locally
at `/video/live.mjpg`. The existing AP,
hostapd, DHCP, and driver setup is deliberately outside this change.

For a prefix of `/opt/t113`, the complete service can be started with:

```sh
/opt/t113/bin/start_race.sh
```

`race_core` stays in PRECHECK while either CAN interface or the logger mount is
missing. `race_video` and `webd` are started only after the core reports
RUNNING. Video recording is latched when VehicleState reports
`hv_voltage_v > 300V`; live preview is available before recording starts.

After starting `race_video`, connect a browser or phone to the existing T113
AP and open `http://<board-ip>/`. The dashboard shows the video card only when
the source is fresh. The video route is local-only, capped at four clients,
and never controls the camera or recording path.
