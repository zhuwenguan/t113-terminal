# race_core

Phase 1 keeps the validated CAN, LapPro, lap/delta/accel, HMI and logger source
files in `src/` and assembles them as `race_core`. Video is never started by
this target and no video source object is linked into it. Moving source files
physically is deferred to avoid mixing path churn with fault-domain extraction.
