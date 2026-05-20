#pragma once

#include <Arduino.h>
#include <FastLED.h>

#include "config.h"

// =====================================================================
// Procedural grayscale 3D noise sampler for idle mode
// =====================================================================
//
// Uses FastLED's inoise8() gradient noise (simplex-family, returns a
// uint8_t given uint16_t x/y/z). The renderer in Seesaw.ino sweeps
// every (strip, LED) on every idle tick, advances a shared z by
// IDLE_NOISE_Z_STEP, and writes the (contrast-stretched) sample as a
// grayscale pixel.
//
// Field coordinates:
//   x = LED position along the strip * IDLE_NOISE_X_SCALE. inoise8's
//       feature length is ~64-128 noise units, so X_SCALE controls how
//       many distinct blobs you see across the strip:
//         X_SCALE=10  -> 1-2 blobs across 45 LEDs (looks uniform)
//         X_SCALE=40  -> 5-10 blobs (visible per-LED variation)
//         X_SCALE=80  -> sparkly per-pixel noise (no blobs)
//   y = strip index * IDLE_NOISE_Y_STRIDE + IDLE_NOISE_Y_BASE so the
//       four strips read different slices of the field. Set
//       IDLE_NOISE_Y_STRIDE = 0 to lock all four strips identical.
//   z = time. Advanced by IDLE_NOISE_Z_STEP per idle tick. uint16_t
//       wraps cleanly (inoise8 is continuous across wraps), so the
//       animation runs indefinitely with no visible reset.
//
// Per-seesaw spatial offset (IDLE_NOISE_PER_ID_OFFSET * SEESAW_ID) is
// added to x and y so neighboring seesaws don't appear in lockstep.
//
// Contrast stretch:
//   inoise8 output is concentrated in the middle of the 0..255 range
//   and rarely hits the extremes. Without remapping, every pixel sits
//   between brightness ~60 and ~190 and the strip looks like solid
//   warm white. This sampler remaps [IDLE_NOISE_FLOOR..IDLE_NOISE_CEIL]
//   to [0..255] (saturating outside) so the field actually reaches
//   fully dark gaps and fully bright peaks.
// =====================================================================

static inline uint8_t stretchIdleNoise(uint8_t v) {
  if (v <= (uint8_t)IDLE_NOISE_FLOOR) return 0;
  if (v >= (uint8_t)IDLE_NOISE_CEIL)  return 255;
  const uint16_t numer = (uint16_t)(v - (uint8_t)IDLE_NOISE_FLOOR) * 255U;
  const uint16_t denom = (uint16_t)((uint8_t)IDLE_NOISE_CEIL
                                    - (uint8_t)IDLE_NOISE_FLOOR);
  return (uint8_t)(numer / denom);
}

static inline uint8_t sampleIdleNoise(uint8_t  stripIndex,
                                      uint16_t ledIndex,
                                      uint16_t z) {
  const uint16_t idBias = (uint16_t)((uint16_t)SEESAW_ID
                                     * (uint16_t)IDLE_NOISE_PER_ID_OFFSET);
  const uint16_t x = (uint16_t)((uint16_t)ledIndex * (uint16_t)IDLE_NOISE_X_SCALE)
                     + idBias;
  const uint16_t y = (uint16_t)((uint16_t)stripIndex * (uint16_t)IDLE_NOISE_Y_STRIDE)
                     + (uint16_t)IDLE_NOISE_Y_BASE
                     + idBias;
  return stretchIdleNoise(inoise8(x, y, z));
}
