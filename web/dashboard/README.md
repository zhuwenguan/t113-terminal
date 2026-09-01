# dashboard

The static dashboard is served by `webd`. It opens the native MJPEG stream at
`/video/live.mjpg` only while `/api/system.video.status` is `live`, and shows
an offline/stale placeholder otherwise. The camera card is hidden when
`/api/capabilities.video` is false.

`bms.html` is the BMS Info page. It polls `/api/bms` and renders the KPE601
meter, four KPL07 boards, all 96 cells, eight live temperatures per board, and
the bounded edge-detected fault event log.
