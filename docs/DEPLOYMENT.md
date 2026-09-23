# Deployment

Status: onboard hardening implemented; board fault-injection validation pending

The T113 install includes BusyBox SysV startup assets under `etc/init.d`,
`race_core`, `race_video`, `webd`, `t113_watchdog`, health logging, and the race
supervisor. Enable `S95t113-terminal` (or the compatibility `S75race`) in the
board's init sequence; enable one of these names, not both. See [Onboard hardening](ONBOARD_HARDENING.md) for the
release/install/rollback procedure.

The supervisor keeps HMI/CAN active while the core is in PRECHECK and starts
video/web only after `/race_core_status` reports RUNNING. A real board still
needs CAN interface setup, the `/mnt/usb/sda` mount, and a power hold-up design
if instantaneous low-voltage removal must be covered by the data-retention
guarantee.
