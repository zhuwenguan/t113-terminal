# terminal-core

This application contains the current validated single-board runtime. The
sources under `src/` cover CAN, LapPro, lap/delta/accel, HMI, logger, replay and
state management; `include/` contains their process-private interfaces.

The `video/`, `hud/` and `ui/` directories contain the existing 800x600 video
and HUD pipeline. The `race_core` target never starts video, and no video
source object is linked into it.
