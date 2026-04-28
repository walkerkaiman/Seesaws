// =====================================================================
// Seesaw firmware (Teensy 4.0)
// =====================================================================
//
// One ball/mercury tilt switch picks between SIDE_A and SIDE_B. On every
// debounced state change the firmware:
//
//   1. Sends a 6-byte event frame over RS485 (announcing this seesaw's
//      ID and the new direction) to the Pi audio player.
//
//   2. Plays the chase animation on both LED strips simultaneously,
//      forward for SIDE_A and reverse for SIDE_B. A new tilt event
//      interrupts an in-progress chase.
//
// Hardware:
//   - Teensy 4.0 powered from a per-seesaw 5V PSU on VIN.
//   - WS2813 strips driven via a 74AHCT125 (5V) buffer.
//   - MAX3485 (3.3V) RS485 transceiver on Serial1 with DE/RE on
//     PIN_RS485_DE; bus carries A, B and a GND reference wire.
//
// Per-board configuration: edit SEESAW_ID in config.h before flashing.
// =====================================================================

#include <Arduino.h>
#include <WS2812Serial.h>

#include "config.h"
#include "protocol.h"
#include "chase.h"

// ---- LED strips -----------------------------------------------------

byte           led1Drawing[CHASE_NUM_LEDS * 3];
DMAMEM byte    led1Display[CHASE_NUM_LEDS * 12];
WS2812Serial   leds1(CHASE_NUM_LEDS, led1Display, led1Drawing,
                     PIN_LED_STRIP_1, WS2811_GRB);

byte           led2Drawing[CHASE_NUM_LEDS * 3];
DMAMEM byte    led2Display[CHASE_NUM_LEDS * 12];
WS2812Serial   leds2(CHASE_NUM_LEDS, led2Display, led2Drawing,
                     PIN_LED_STRIP_2, WS2811_GRB);

// ---- Tilt state -----------------------------------------------------

int            lastStableTilt = -1;
int            lastReadTilt   = -1;
elapsedMillis  tiltDebounceTimer;

// ---- Chase playback -------------------------------------------------

bool           chaseActive = false;
int            chaseFrame  = 0;
int            chaseStep   = 1;        // +1 forward, -1 reverse
elapsedMillis  frameTimer;
const uint16_t FRAME_INTERVAL_MS = 1000 / CHASE_FPS;

// ---- RS485 ----------------------------------------------------------

uint8_t        txSeq = 0;

// ---- Forward decls --------------------------------------------------

static void pollTilt();
static void onTiltChange(int newState);
static void sendEvent(uint8_t direction);
static void startChase(uint8_t direction);
static void tickChase();
static void drawFrame(int frameIndex);
static void clearStrips();
static void showStrips();

// ---- Setup / loop ---------------------------------------------------

void setup() {
  pinMode(PIN_TILT, INPUT_PULLUP);

  Serial1.begin(RS485_BAUD);
  Serial1.transmitterEnable(PIN_RS485_DE);

  leds1.begin();
  leds2.begin();
  clearStrips();
  showStrips();

  lastStableTilt = digitalRead(PIN_TILT);
  lastReadTilt   = lastStableTilt;
  tiltDebounceTimer = 0;

  randomSeed(analogRead(A0) ^ micros());
}

void loop() {
  pollTilt();
  tickChase();
}

// ---- Tilt -----------------------------------------------------------

static void pollTilt() {
  int current = digitalRead(PIN_TILT);
  if (current != lastReadTilt) {
    lastReadTilt = current;
    tiltDebounceTimer = 0;
    return;
  }
  if (current != lastStableTilt && tiltDebounceTimer >= TILT_DEBOUNCE_MS) {
    lastStableTilt = current;
    onTiltChange(current);
  }
}

static void onTiltChange(int newState) {
  uint8_t direction = (newState == LOW) ? DIR_A : DIR_B;
  sendEvent(direction);
  startChase(direction);
}

// ---- RS485 transmit -------------------------------------------------

static void sendEvent(uint8_t direction) {
  uint8_t seq = txSeq++;
  uint8_t buf[FRAME_SIZE];
  buildFrame(buf, SEESAW_ID, direction, seq);
  for (int i = 0; i < RS485_RESEND_COUNT; i++) {
    Serial1.write(buf, FRAME_SIZE);
    Serial1.flush();
    if (i < RS485_RESEND_COUNT - 1) {
      uint16_t jitter = random(RS485_RESEND_JITTER_MIN_MS,
                               RS485_RESEND_JITTER_MAX_MS + 1);
      delay(jitter);
    }
  }
}

// ---- Chase playback -------------------------------------------------

static void startChase(uint8_t direction) {
  if (direction == DIR_A) {
    chaseFrame = 0;
    chaseStep  = 1;
  } else {
    chaseFrame = CHASE_NUM_FRAMES - 1;
    chaseStep  = -1;
  }
  chaseActive = true;
  frameTimer  = 0;
  drawFrame(chaseFrame);
}

static void tickChase() {
  if (!chaseActive) return;
  if (frameTimer < FRAME_INTERVAL_MS) return;
  frameTimer -= FRAME_INTERVAL_MS;

  int next = chaseFrame + chaseStep;
  if (next < 0 || next >= (int)CHASE_NUM_FRAMES) {
    chaseActive = false;
    clearStrips();
    showStrips();
    return;
  }
  chaseFrame = next;
  drawFrame(chaseFrame);
}

static void drawFrame(int frameIndex) {
  for (int i = 0; i < (int)CHASE_NUM_LEDS; i++) {
    uint8_t r = pgm_read_byte(&chase[frameIndex][i * 3 + 0]);
    uint8_t g = pgm_read_byte(&chase[frameIndex][i * 3 + 1]);
    uint8_t b = pgm_read_byte(&chase[frameIndex][i * 3 + 2]);
    leds1.setPixel(i, r, g, b);
    leds2.setPixel(i, r, g, b);
  }
  showStrips();
}

static void clearStrips() {
  for (int i = 0; i < (int)CHASE_NUM_LEDS; i++) {
    leds1.setPixel(i, 0, 0, 0);
    leds2.setPixel(i, 0, 0, 0);
  }
}

static void showStrips() {
  leds1.show();
  leds2.show();
}
