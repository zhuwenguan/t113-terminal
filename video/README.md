# race_video

`video/src/` owns the validated 800x600 capture, G2D, HUD, JPEG/MP4 and DMA/ION
pipeline. `video/include/` owns its process-private interfaces and vendor ABI
headers. The daemon entry point is `video/main.c`.
