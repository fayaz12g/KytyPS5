# Monitor-Aware Aspect-Ratio Support

Technical notes for a human-authored PR description, in accordance with the repository's
[AI use policy](README.md#ai-use). Verification below describes automated checks, not human
review or live gameplay validation.

## Overview

Adds configurable aspect-ratio scaling to presentation, with centered pillarboxing and
letterboxing, monitor-aware launch sizing, and persistent launcher settings.

## Problem

Previously, presentation filled the entire window without an aspect-ratio constraint. The
initial implementation also had several issues:

- Aspect selections were dropped when copying configuration, causing settings to reset on
  reopening or launching.
- Window sizing and presentation used different interpretations of 21:9.
- The effective ultrawide ratio changed with resolution, introducing unexpected bars in
  fullscreen.
- Monitor detection could use the launcher's display while SDL opened the emulator on the
  primary display.
- Existing settings could silently acquire different dimensions after migration.

## User-Facing Changes

- Adds `Auto`, `Stretch`, `16:9`, `21:9`, `32:9`, `16:10`, and `4:3` aspect options.
- Adds an automatic resolution option using the launcher monitor's desktop dimensions.
- Updates fixed-resolution labels to reflect the selected aspect ratio while preserving the
  selected height.
- Saves and restores aspect selections through configuration copying, settings serialization,
  and game-settings import/export.
- Keeps aspect-ratio and shader-optimization controls in separate settings-layout rows.

For a 3440x1440 monitor, Auto resolution with Auto or 21:9 aspect uses the whole display.
Selecting 16:9 instead produces centered pillarboxing.

## Presentation

- Introduces shared integer-based sizing and presentation-rectangle calculations.
- Fits the selected ratio within the swapchain extent and centers the result.
- Handles empty surfaces and clamps rounded dimensions to valid bounds.
- Clears the destination to black only when boxing is needed, avoiding unnecessary clears
  for full-window presentation.
- Adds a transfer-write dependency between the clear and blit.
- Applies the same content rectangle to guest overlays, keeping them aligned with the
  primary image.

## Ultrawide Handling

The 21:9 option represents the ultrawide display family rather than literal 21 divided by 9.
It uses a matching monitor's exact ratio, with a consistent 43:18 fallback.

Resolution changes therefore affect size, not the selected display ratio. On 3440x1440,
fixed-height options resolve to 1720x720, 2580x1080, 3440x1440, and 5160x2160.

## Monitor Selection

- Passes the launcher monitor's bounds through the emulator arguments.
- Adds `--display <x> <y> <width> <height>`, including negative desktop coordinates.
- Selects the corresponding SDL display before creating the window, including fullscreen
  creation.
- Retains the monitor dimensions as the presentation aspect reference.
- Validates supplied bounds and rejects malformed, zero-sized, or overflowing values.

## Compatibility

- Existing explicit aspect choices remain unchanged.
- Legacy settings without an aspect field retain their previous fixed dimensions and stretch
  behavior.
- New configurations default to automatic resolution and aspect selection.
- The standalone CLI aspect default remains 16:9.
- No README changes, committed tests, or build-system changes are included.

## Verification

- macOS launcher and emulator build successfully.
- Temporary geometry probes passed across three ultrawide display sizes and four resolution
  heights.
- Production configuration probes verified fresh defaults, legacy migration, explicit
  selections, and persistence.
- Eleven valid/invalid display-argument checks passed.
- Changed files have no reported diagnostics; whitespace checks pass.

## Remaining Validation

Live gameplay, multi-monitor/fullscreen behavior, and Windows/Linux builds remain unverified.
Aspect scaling does not modify a game's camera or provide native widescreen rendering.

Before submission, supply the affected game, before/after evidence, the scope of AI assistance,
and human review/testing details, as required by the
[contribution policy](README.md#contributing) and [AI use policy](README.md#ai-use).