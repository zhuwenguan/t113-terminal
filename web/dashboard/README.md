# dashboard

The static dashboard is served by `webd`. It opens the native MJPEG stream at
`/video/live.mjpg` only while `/api/system.video.status` is `live`, and shows
an offline/stale placeholder otherwise. The camera card is hidden when
`/api/capabilities.video` is false.
