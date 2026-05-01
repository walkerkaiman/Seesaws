#pragma once
#include <Arduino.h>

// =====================================================================
// CHASE DATA
// =====================================================================
//
// The chase animation. The firmware lights only the pair of strips on
// the side that just bottomed out: a DIR_A event plays this data
// forward (frame 0 -> CHASE_NUM_FRAMES-1) on the SIDE_A pair; a DIR_B
// event plays it in reverse (CHASE_NUM_FRAMES-1 -> 0) on the SIDE_B
// pair. The other pair stays dark for that chase. Both pins inside the
// active pair receive identical pixel data so the two strips on that
// side animate in lock-step.
//
// Format: one row per animation frame, CHASE_NUM_LEDS triplets per row,
// in R,G,B order, values 0..255.
//
// Two ways to update this file:
//
//   1) Run Firmware/tools/csv_to_header.py path/to/chase.csv
//      (overwrites this file with the contents of the CSV).
//
//   2) Manually update CHASE_NUM_LEDS and CHASE_NUM_FRAMES below, then
//      replace the rows between the BEGIN/END markers with your data.
//      Each row is { r,g,b, r,g,b, ... } with CHASE_NUM_LEDS triplets,
//      and there must be exactly CHASE_NUM_FRAMES rows.
//
// The placeholder below is a single red dot walking across 5 LEDs over
// 5 frames, just so the firmware compiles and you can verify behavior
// on the bench. Replace it with your real chase data.
// =====================================================================

#define CHASE_NUM_LEDS    5
#define CHASE_NUM_FRAMES  5

const uint8_t chase[CHASE_NUM_FRAMES][CHASE_NUM_LEDS * 3] PROGMEM = {
  // ========== BEGIN CHASE DATA ==========
  {  64,  0,  0,    0,  0,  0,    0,  0,  0,    0,  0,  0,    0,  0,  0 },
  {   0,  0,  0,   64,  0,  0,    0,  0,  0,    0,  0,  0,    0,  0,  0 },
  {   0,  0,  0,    0,  0,  0,   64,  0,  0,    0,  0,  0,    0,  0,  0 },
  {   0,  0,  0,    0,  0,  0,    0,  0,  0,   64,  0,  0,    0,  0,  0 },
  {   0,  0,  0,    0,  0,  0,    0,  0,  0,    0,  0,  0,   64,  0,  0 },
  // ==========  END CHASE DATA  ==========
};
