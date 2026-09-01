# T113 deployment

The Phase 1 install includes `race_core`, `webd`, `race_video`, the sample
`config/webd.conf`, and the static dashboard under `web/`. The existing AP,
hostapd, DHCP, and driver setup is deliberately outside this change.

For a prefix of `/opt/t113`, the local service can be started with:

```sh
/opt/t113/bin/webd --config /opt/t113/config/webd.conf \
  --web-root /opt/t113/web
```
