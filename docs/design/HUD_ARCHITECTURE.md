# HUD Architecture

The production HUD is a fixed 800x600 page rendered into the caller-owned
T113 ARGB8888 surface. The layout definition is frozen in
`HUD_LAYOUT_800x600_DESIGNED.md` and represented by `src/hud/hud_layout.h`.

```text
VehicleState
    ↓
hud_render
    ↓
hud_widget
    ↓
ui_draw / ui_font / ui_image
    ↓
HudSurface
    ↓
G2D
```

## Runtime boundaries

- `video/src/hud_renderer.c` owns the HudSurface bridge, renderer lifecycle,
  and monotonic render timing.
- `src/hud/hud_render.c` binds VehicleState values to the fixed page and owns
  page-level draw order and formatting.
- `src/hud/hud_widget.c` provides reusable, business-data-independent panels,
  text/value rows, segment bars, and status indicators.
- `src/ui/ui_draw.c` provides CPU-rasterized integer geometry with clipping.
- `src/ui/ui_font.c` and `video/src/hud_text.c` provide FreeType text and the
  fixed-capacity glyph cache.
- `src/ui/ui_image.c` provides optional PNG binary-alpha loading and drawing;
  the fixed page uses the PNG icons in `assets/icons/` for timer, battery,
  power, motor temperature, controller temperature, and mode.

The normal lifecycle is:

```text
hud_renderer_init_style()
    → hud_init() / font warm-up
    → hud_renderer_render_frame_to_surface() repeatedly
    → hud_renderer_close() / hud_shutdown()
```

All production HUD rendering is CPU rasterization into an ARGB8888 surface;
G2D performs the subsequent composition. CPU-visible ARGB8888 bytes are
`B G R A`. The production resolution is fixed at 800x600.

## Current release status

- Status: Production / maintenance mode.
- Resolution: 800x600 at a 30 FPS target.
- Render architecture: one initialized static HUD cache plus a per-frame
  dynamic data overlay.
- Typical T113 HUD cost: approximately 3.5 ms/frame.
- Static cache memory: one additional 1,920,000-byte 800x600 ARGB8888 buffer.

Tests must not depend on `assert()` side effects because Release builds define
`NDEBUG`; test conditions and required calls use `tests/test_check.h` instead.

## Change entry points

- Position, size, spacing: `HUD_LAYOUT_800x600_DESIGNED.md` and
  `src/hud/hud_layout.h`.
- Colors and visual semantics: `src/hud/hud_theme.h`.
- Vehicle data display and draw order: `src/hud/hud_render.c`.
- Reusable widget behavior: `src/hud/hud_widget.c/.h`.
- Primitive rasterization and clipping: `src/ui/ui_draw.c/.h`.
- Font and glyph-cache behavior: `src/ui/ui_font.c/.h` and
  `video/src/hud_text.c`.
- Optional PNG resources: `src/ui/ui_image.c/.h`.
- HudSurface/G2D integration: `video/src/hud_renderer.c` and
  `video/src/video_pipeline.c`.

`hud_render()` does not load resources or allocate memory per frame. Fonts
are loaded and warmed during initialization; the glyph cache is fixed at 384
entries. Static page content is prepared during initialization and copied into
the caller surface before dynamic values are drawn. Widgets receive drawing
parameters only and do not depend on VehicleState, CAN, or LapPro.
