# race_core

`core/src/` contains the validated CAN, LapPro, lap/delta/accel, HMI and logger
implementation. `core/include/` contains its process-private interfaces.
`race_core` never starts video and no video source object is linked into it.
