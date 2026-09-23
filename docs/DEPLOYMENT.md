# Deployment

Status: onboard hardening implemented; board fault-injection validation pending

The T113 install includes BusyBox SysV startup assets under `etc/init.d`,
`race_core`, `race_video`, `webd`, `t113_watchdog`, health logging, the race
supervisor, and the AP setup helper. Enable `S95t113-terminal` (or the
compatibility `S75race`) in the board's init sequence; enable one of these
names, not both. See [Onboard hardening](ONBOARD_HARDENING.md) for the
release/install/rollback procedure.

The supervisor keeps HMI/CAN active while the core is in PRECHECK and starts
video/web only after `/race_core_status` reports RUNNING. A real board still
needs CAN interface setup, the `/mnt/usb/sda` mount, and a power hold-up design
if instantaneous low-voltage removal must be covered by the data-retention
guarantee. `start_race.sh` invokes `t113_ap.sh` before starting the race
supervisor, so the stock station-mode WiFi service is converted to the local AP
on every boot and manual restart. The helper uses the board's existing
`/etc/hostapd.conf` and `/etc/dnsmasq.conf`.
