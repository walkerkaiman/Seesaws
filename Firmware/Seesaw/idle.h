#pragma once
#include <Arduino.h>

// =====================================================================
// IDLE CHASE DATA
// =====================================================================
//
// The idle animation. Played continuously on all four LED strips when
// the seesaw is idle (no tilt events for IDLE_TIMEOUT_MS, default 60s).
// Each strip plays this same animation but shifted by
// IDLE_FRAME_OFFSET_<strip> frames (see config.h), creating a
// phase-shifted wave across the four strips. The animation loops as
// long as the seesaw stays idle; the first tilt event drops it and
// switches the firmware into PLAY mode.
//
// Format: one row per animation frame, IDLE_NUM_LEDS triplets per row,
// in R,G,B order, values 0..255. Same on-disk format as chase.h.
//
// Two ways to update this file:
//
//   1) Run Firmware/tools/csv_to_header.py path/to/idle.csv --target idle
//      (overwrites this file with the contents of the CSV).
//
//   2) Manually update IDLE_NUM_LEDS and IDLE_NUM_FRAMES below, then
//      replace the rows between the BEGIN/END markers with your data.
//      Each row is { r,g,b, r,g,b, ... } with IDLE_NUM_LEDS triplets,
//      and there must be exactly IDLE_NUM_FRAMES rows.
//
// The placeholder is a slow red breath - all LEDs ramp up, peak, and
// ramp back down over 8 frames - just so the firmware compiles and you
// can verify behavior on the bench. With the default per-strip frame
// offsets in config.h (0, N/4, 2N/4, 3N/4) the four strips will breathe
// out of phase, so you can see the wave effect immediately. Replace it
// with your real idle animation via the CSV tool.
// =====================================================================

#define IDLE_NUM_LEDS    5
#define IDLE_NUM_FRAMES  8

const uint8_t idle[IDLE_NUM_FRAMES][IDLE_NUM_LEDS * 3] PROGMEM = {
  // ========== BEGIN IDLE DATA ==========
  {   0,  0,  0,    0,  0,  0,    0,  0,  0,    0,  0,  0,    0,  0,  0 },
  {  16,  0,  0,   16,  0,  0,   16,  0,  0,   16,  0,  0,   16,  0,  0 },
  {  32,  0,  0,   32,  0,  0,   32,  0,  0,   32,  0,  0,   32,  0,  0 },
  {  48,  0,  0,   48,  0,  0,   48,  0,  0,   48,  0,  0,   48,  0,  0 },
  {  64,  0,  0,   64,  0,  0,   64,  0,  0,   64,  0,  0,   64,  0,  0 },
  {  48,  0,  0,   48,  0,  0,   48,  0,  0,   48,  0,  0,   48,  0,  0 },
  {  32,  0,  0,   32,  0,  0,   32,  0,  0,   32,  0,  0,   32,  0,  0 },
  {  16,  0,  0,   16,  0,  0,   16,  0,  0,   16,  0,  0,   16,  0,  0 },
  // ==========  END IDLE DATA  ==========
};
