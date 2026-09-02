# Deployment

Status: Phase 1 implemented

The T113 install includes BusyBox SysV startup assets under `etc/init.d`,
`race_core`, `race_video`, `webd`, and the race supervisor. Run
`/opt/t113/bin/start_race.sh` after installing the prefix, or enable
`/opt/t113/etc/init.d/S75race` in the board's init sequence.

The supervisor keeps HMI/CAN active while the core is in PRECHECK and starts
video/web only after `/race_core_status` reports RUNNING. A real board still
needs CAN interface setup, the `/mnt/usb/sda` mount, and a power hold-up design
if instantaneous low-voltage removal must be covered by the data-retention
guarantee.
