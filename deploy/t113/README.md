# T113 deployment

The Phase 1 install includes `race_core`, `webd`, `race_video`, the sample
`config/webd.conf`, and the static dashboard under `web/`. `race_video` publishes
the original camera MJPEG through `/race_latest_jpeg`; `webd` serves it locally
at `/video/live.mjpg`. The existing AP,
hostapd, DHCP, and driver setup is deliberately outside this change.

For a prefix of `/opt/t113`, the local service can be started with:

```sh
/opt/t113/bin/webd --config /opt/t113/config/webd.conf \
  --web-root /opt/t113/web
```

After starting `race_video`, connect a browser or phone to the existing T113
AP and open `http://<board-ip>/`. The dashboard shows the video card only when
the source is fresh. The video route is local-only, capped at four clients,
and never controls the camera or recording path.
