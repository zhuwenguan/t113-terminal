# video-ipc

`race_video_ipc` provides the local latest-frame transport for the camera
旁路. It uses a POSIX shared-memory object named `/race_latest_jpeg` by
default, with two fixed 4 MiB JPEG slots. The producer writes the inactive
slot, publishes it with a release-ordered active index, and never waits for a
reader. Readers validate the slot seqlock before accepting a copied frame.

The IPC carries original JPEG bytes and timing/counter metadata. It does not
decode, scale, composite HUD, or encode frames.
