// =====================================================================
// Seesaws central audio node (Teensy 3.2 + PJRC Audio Shield)
// =====================================================================
//
// Listens on RS485 for tilt events from the seesaw firmware and plays
// the matching WAV from the Audio Shield SD card. Multiple sounds overlap
// (polyphonic) - a new tilt never cuts off a sound already playing.
//
// Sound files on the SD card (under SOUNDS_DIR in config.h):
//
//   sounds/1_A.wav   plays when seesaw id 1, SIDE_A (event 0)
//   sounds/1_B.wav   plays when seesaw id 1, SIDE_B (event 1)
//   sounds/2_A.wav   ...
//
// The numeric prefix must match the seesaw's SEESAW_ID in firmware.
// No config file or code change is needed to add a seesaw - drop in WAVs.
//
// Hardware:
//   - Teensy 3.2 + Audio Shield (line-out or headphone to your amp)
//   - MAX3485 on Serial1: RX pin 0, TX pin 1, DE+RE pin 2
//   - Bias + termination at this (rack) end of the RS485 bus
//
// Libraries (Teensyduino): Audio, SD, SPI, SerialFlash (installed with Audio)
// =====================================================================

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>

#include "config.h"
#include "protocol.h"

// ---- Audio graph: MAX_VOICES players -> mixers -> I2S out ------------

AudioPlaySdWav play0;
AudioPlaySdWav play1;
AudioPlaySdWav play2;
AudioPlaySdWav play3;
AudioPlaySdWav play4;
AudioPlaySdWav play5;
AudioPlaySdWav play6;
AudioPlaySdWav play7;

AudioPlaySdWav *const kPlayers[MAX_VOICES] = {
  &play0, &play1, &play2, &play3, &play4, &play5, &play6, &play7,
};

#if MAX_VOICES != 8
#error "This sketch wires exactly 8 AudioPlaySdWav instances; adjust MAX_VOICES and the player/mixer graph together."
#endif

AudioMixer4 mixA;
AudioMixer4 mixB;
AudioMixer2 mixOut;
AudioOutputI2S audioOut;

AudioConnection pc00(play0, 0, mixA, 0);
AudioConnection pc01(play1, 0, mixA, 1);
AudioConnection pc02(play2, 0, mixA, 2);
AudioConnection pc03(play3, 0, mixA, 3);
AudioConnection pc04(play4, 0, mixB, 0);
AudioConnection pc05(play5, 0, mixB, 1);
AudioConnection pc06(play6, 0, mixB, 2);
AudioConnection pc07(play7, 0, mixB, 3);
AudioConnection pc10(mixA, 0, mixOut, 0);
AudioConnection pc11(mixB, 0, mixOut, 1);
AudioConnection pc20(mixOut, 0, audioOut, 0);
AudioConnection pc21(mixOut, 0, audioOut, 1);

// ---- RS485 frame parser ----------------------------------------------

static void handleEvent(uint8_t id, uint8_t event, uint8_t seq);

static uint8_t rxBuf[64];
static size_t rxLen = 0;

// Dedupe: last (id, seq) pairs from each seesaw (simple ring per id slot).
static const size_t DEDUPE_SLOTS = 32;
static uint8_t dedupeId[DEDUPE_SLOTS];
static uint8_t dedupeSeq[DEDUPE_SLOTS];
static size_t dedupeHead = 0;

static bool dedupeSeen(uint8_t id, uint8_t seq) {
  for (size_t i = 0; i < DEDUPE_SLOTS; i++) {
    if (dedupeId[i] == id && dedupeSeq[i] == seq) return true;
  }
  dedupeId[dedupeHead] = id;
  dedupeSeq[dedupeHead] = seq;
  dedupeHead = (dedupeHead + 1) % DEDUPE_SLOTS;
  return false;
}

static void consumeByte(uint8_t b) {
  if (rxLen >= sizeof(rxBuf)) {
    rxLen = 0;
  }
  rxBuf[rxLen++] = b;

  while (rxLen >= FRAME_SIZE) {
    size_t i = 0;
    for (; i + 1 < rxLen; i++) {
      if (rxBuf[i] == FRAME_SOF1 && rxBuf[i + 1] == FRAME_SOF2) break;
    }
    if (i > 0) {
      memmove(rxBuf, rxBuf + i, rxLen - i);
      rxLen -= i;
    }
    if (rxLen < FRAME_SIZE) return;

    const uint8_t id = rxBuf[2];
    const uint8_t event = rxBuf[3];
    const uint8_t seq = rxBuf[4];
    const uint8_t recvCrc = rxBuf[5];
    const uint8_t calcCrc = crc8(&rxBuf[2], 3);

    if (calcCrc != recvCrc) {
      Serial.print("CRC mismatch got 0x");
      Serial.print(recvCrc, HEX);
      Serial.print(" want 0x");
      Serial.println(calcCrc, HEX);
      memmove(rxBuf, rxBuf + 1, rxLen - 1);
      rxLen--;
      continue;
    }

    memmove(rxBuf, rxBuf + FRAME_SIZE, rxLen - FRAME_SIZE);
    rxLen -= FRAME_SIZE;

    if (dedupeSeen(id, seq)) {
      Serial.print("Dedup seesaw ");
      Serial.print(id);
      Serial.print(" seq ");
      Serial.println(seq);
      continue;
    }

    handleEvent(id, event, seq);
  }
}

static void buildSoundPath(char *out, size_t outLen, uint8_t id, uint8_t direction) {
  const char side = (direction == DIR_A) ? 'A' : 'B';
  snprintf(out, outLen, SOUNDS_DIR "/%u_%c.wav", (unsigned)id, side);
}

static AudioPlaySdWav *findFreePlayer() {
  for (size_t i = 0; i < MAX_VOICES; i++) {
    if (!kPlayers[i]->isPlaying()) return kPlayers[i];
  }
  return nullptr;
}

static void playTilt(uint8_t id, uint8_t direction) {
  char path[48];
  buildSoundPath(path, sizeof(path), id, direction);

  if (!SD.exists(path)) {
    Serial.print("Missing sound: ");
    Serial.println(path);
    return;
  }

  AudioPlaySdWav *player = findFreePlayer();
  if (player == nullptr) {
    Serial.print("No free voice for ");
    Serial.println(path);
    return;
  }

  player->play(path);
  Serial.print("Playing ");
  Serial.println(path);
}

// State-change stub - same role as on_state_change() in the old Pi player.
static void onStateChange(uint8_t id, uint8_t event, uint8_t seq) {
  Serial.print("State change: seesaw ");
  Serial.print(id);
  Serial.print(" -> ");
  if (event == EVT_STATE_IDLE) Serial.print("STATE_IDLE");
  else if (event == EVT_STATE_PLAY) Serial.print("STATE_PLAY");
  else Serial.print(event);
  Serial.print(" (seq ");
  Serial.print(seq);
  Serial.println(")");
}

static void handleEvent(uint8_t id, uint8_t event, uint8_t seq) {
  if (event == EVT_TILT_A || event == EVT_TILT_B) {
    playTilt(id, event);
  } else if (event == EVT_STATE_IDLE || event == EVT_STATE_PLAY) {
    onStateChange(id, event, seq);
  } else {
    Serial.print("Unknown event 0x");
    Serial.print(event, HEX);
    Serial.print(" from seesaw ");
    Serial.print(id);
    Serial.print(" (seq ");
    Serial.print(seq);
    Serial.println(")");
  }
}

static void initMixers() {
  for (int i = 0; i < 4; i++) {
    mixA.gain(i, 1.0f);
    mixB.gain(i, 1.0f);
  }
  mixOut.gain(0, 0.5f);
  mixOut.gain(1, 0.5f);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { /* USB console */ }

  AudioMemory(AUDIO_MEMORY_BLOCKS);
  initMixers();

  if (!SD.begin()) {
    Serial.println("SD card init failed - check card and Audio Shield seating");
    while (1) delay(1000);
  }
  Serial.println("SD card ready");

  Serial1.begin(RS485_BAUD);
  Serial1.transmitterEnable(PIN_RS485_DE);

  Serial.print("Seesaw audio listening @ ");
  Serial.print(RS485_BAUD);
  Serial.print(" baud, ");
  Serial.print(MAX_VOICES);
  Serial.print(" voices, sounds in ");
  Serial.println(SOUNDS_DIR "/");
}

void loop() {
  while (Serial1.available()) {
    consumeByte((uint8_t)Serial1.read());
  }
}
