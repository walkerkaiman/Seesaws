#pragma once

// =====================================================================
// Central audio node configuration (Teensy 3.2 + Audio Shield)
// =====================================================================

// RS485 on Serial1: RX=0, TX=1, DE+RE=2 (same as seesaw firmware).
// Do not use Serial2 (pins 7/8) - those are used by the Audio Shield.
#define PIN_RS485_DE  2
#define RS485_BAUD    115200

// Simultaneous WAV voices. Each voice is one AudioPlaySdWav + mixer input.
// Teensy 3.2 RAM limits practical polyphony; raise only if you have headroom.
#define MAX_VOICES    8

// Audio library DMA buffer count. Increase if you hear dropouts under load.
#define AUDIO_MEMORY_BLOCKS  16

// SD card folder for tilt sounds (leading/trailing slashes optional).
// Files must be named {seesaw_id}_A.wav and {seesaw_id}_B.wav, e.g. 1_A.wav.
#define SOUNDS_DIR    "sounds"
