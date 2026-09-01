# Documentation index

- [Architecture](ARCHITECTURE.md): daemon ownership, IPC ABI and lifecycle.
- [Web API](WEB_API.md): reserved local web API design.
- [Telemetry protocol](TELEMETRY_PROTOCOL.md): reserved remote protocol design.
- [Deployment](DEPLOYMENT.md): reserved T113/cloud deployment design.
- [Usage](USAGE.md): build and runtime commands.
- [T113 project context](T113_project_context.md): board and development context.
- [HUD architecture](design/HUD_ARCHITECTURE.md): HUD rendering boundaries.
- [HUD layout](design/HUD_LAYOUT_800x600_DESIGNED.md): fixed 800×600 layout.
- `design/`: HUD product/design references.
- `investigations/`: engineering investigations and incident records.
- `validation/`: historical DMA, G2D and 800×600 validation records.

Generated build products and host-editor caches are intentionally not kept in
the repository workspace; regenerate them in an ignored `build*` directory.
