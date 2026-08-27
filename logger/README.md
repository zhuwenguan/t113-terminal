# race_logger boundary

The validated logger remains inside `race_core` in phase 1. Its producer/sink
API is the future IPC boundary; storage IPC is deliberately not implemented in
this migration.
