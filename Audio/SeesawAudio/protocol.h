#pragma once
#include <Arduino.h>

// =====================================================================
// RS485 wire protocol (shared with Firmware/Seesaw/protocol.h)
// =====================================================================
//
// Fixed 6-byte little-endian frame, 8N1 at RS485_BAUD:
//
//   byte 0 : 0xAA          start-of-frame 1
//   byte 1 : 0x55          start-of-frame 2
//   byte 2 : id            seesaw id (1..255)
//   byte 3 : event         event code, see EVENT CODES below
//   byte 4 : seq           rolling counter, used for dedupe
//   byte 5 : crc8          CRC-8 (poly 0x07) over bytes 2..4
//
// EVENT CODES (byte 3):
//
//   Tilt events - one side bottomed out (drive SD WAV playback):
//     DIR_A / EVT_TILT_A   0    SIDE_A bottomed out
//     DIR_B / EVT_TILT_B   1    SIDE_B bottomed out
//
//   State-change events - the seesaw's mode just changed:
//     EVT_STATE_IDLE       2    entered IDLE
//     EVT_STATE_PLAY       3    entered PLAY
//
// Tilt events play Audio/sounds/{id}_A.wav or {id}_B.wav on the SD card.
// State-change events are logged only (hook for future idle-aware audio).
//
// Each event is transmitted RS485_RESEND_COUNT times with random jitter.
// This node accepts the first valid (id, seq) pair and ignores duplicates.
// =====================================================================

#define FRAME_SOF1  0xAA
#define FRAME_SOF2  0x55
#define FRAME_SIZE  6

#define DIR_A           0
#define DIR_B           1
#define EVT_TILT_A      DIR_A
#define EVT_TILT_B      DIR_B
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
