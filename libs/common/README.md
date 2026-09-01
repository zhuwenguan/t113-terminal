# common

Process-neutral support lives here. Public headers are namespaced under
`libs/common/include/race/`; `librace_common.a` contains the shared logger and
monotonic-time utility.

The reusable POSIX shared-memory implementation is now kept in `libs/ipc/`,
and the authoritative `VehicleState` ABI is in `libs/vehicle-state/`.
