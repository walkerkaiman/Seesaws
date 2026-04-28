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
//   byte 3 : direction     DIR_A (0) or DIR_B (1)
//   byte 4 : seq           rolling counter, used by the Pi for dedupe
//   byte 5 : crc8          CRC-8 (poly 0x07) over bytes 2..4
//
// Each event is transmitted RS485_RESEND_COUNT times with random jitter.
// The Pi accepts the first valid (id, seq) pair and ignores duplicates.
// =====================================================================

#define FRAME_SOF1  0xAA
#define FRAME_SOF2  0x55
#define FRAME_SIZE  6

#define DIR_A       0
#define DIR_B       1

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
                              uint8_t direction,
                              uint8_t seq) {
  buf[0] = FRAME_SOF1;
  buf[1] = FRAME_SOF2;
  buf[2] = id;
  buf[3] = direction;
  buf[4] = seq;
  buf[5] = crc8(&buf[2], 3);
}
