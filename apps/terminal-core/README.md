# terminal-core

This application contains the current validated single-board runtime. The
sources under `src/` cover CAN, LapPro, lap/delta/accel, HMI, logger, replay and
state management; `include/` contains their process-private interfaces.

The `video/`, `hud/` and `ui/` directories contain the existing 800x600 video
and HUD pipeline. The `race_core` target never starts video, and no video
source object is linked into it.

In the production split, `race_core` publishes `/race_core_status` in addition
to VehicleState. It remains in PRECHECK while either CAN interface or the
logger mount is unavailable, and publishes RUNNING only when both CAN buses
and logger are ready. `race_video` can publish live preview in RUNNING and
creates MP4 segments only after a valid HV voltage strictly greater than 300 V.

Logger batches are flushed with `fflush + fdatasync` at most once per second;
the next start also removes an unterminated CSV tail from prior sessions. This
reduces software-side loss, but an instantaneous low-voltage cut still needs
hardware hold-up power for a strict guarantee.
