#pragma once
#include <Arduino.h>

// =====================================================================
// RS485 wire protocol (shared with Audio/seesaw_audio.py on the Pi)
// =====================================================================
//
// Fixed 6-byte little-endian frame, 8N1 at RS485_BAUD:
//
//   byte 0 : 0xAA          start-of-frame 1
//   byte 1 : 0x55          start-of-frame 2
//   byte 2 : id            seesaw id (1..255)
//   byte 3 : event         event code, see EVENT CODES below
//   byte 4 : seq           rolling counter, used by the Pi for dedupe
//   byte 5 : crc8          CRC-8 (poly 0x07) over bytes 2..4
//
// EVENT CODES (byte 3):
//
//   Tilt events - one side bottomed out (drives audio playback on Pi):
//     DIR_A           0    SIDE_A bottomed out (also EVT_TILT_A)
//     DIR_B           1    SIDE_B bottomed out (also EVT_TILT_B)
//
//   State-change events - the seesaw's mode just changed:
//     EVT_STATE_IDLE  2    just entered IDLE (boot, or PLAY -> IDLE timeout)
//     EVT_STATE_PLAY  3    just entered PLAY (first tilt out of IDLE)
//
//   Tilt events drive the Pi audio player; state-change events are
//   delivered to a listener stub on the Pi (Audio/seesaw_audio.py
//   on_state_change) that currently does nothing, so the Pi can grow
//   idle-aware behavior later (attract music, prompts, etc.) without
//   another firmware change.
//
// Each event is transmitted RS485_RESEND_COUNT times with random jitter.
// The Pi accepts the first valid (id, seq) pair and ignores duplicates.
// =====================================================================

#define FRAME_SOF1  0xAA
#define FRAME_SOF2  0x55
#define FRAME_SIZE  6

// Tilt events (byte 3 = 0..1). DIR_A/DIR_B are the historical names; the
// EVT_TILT_* aliases match the broader "event code" framing.
#define DIR_A           0
#define DIR_B           1
#define EVT_TILT_A      DIR_A
#define EVT_TILT_B      DIR_B

// State-change events (byte 3 = 2..3). Sent on every IDLE<->PLAY
// transition. The Pi has a listener stub for these; no action is taken
// today, but the wire path is in place.
#define EVT_STATE_IDLE  2
#define EVT_STATE_PLAY  3

static inline uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x80) crc = (uint8_t)((crc << 1) ^ 0x07);
      else            crc = (uint8_t)(crc << 1);
    }
  }
  return crc;
}

static inline void buildFrame(uint8_t buf[FRAME_SIZE],
                              uint8_t id,
                              uint8_t event,
                              uint8_t seq) {
  buf[0] = FRAME_SOF1;
  buf[1] = FRAME_SOF2;
  buf[2] = id;
  buf[3] = event;
  buf[4] = seq;
  buf[5] = crc8(&buf[2], 3);
}
