// =====================================================================
// Seesaw firmware (Teensy 4.0)
// =====================================================================
//
// An MPU6050 measures the seesaw's angular velocity on the configured
// rotation axis. Events fire on direction reversal - the moment one side
// reaches its lowest point and starts coming back up. This is the impact
// moment, and it works at any amplitude (small kids and adults trigger
// the same way). On every event the firmware:
//
//   1. Sends a 6-byte event frame over RS485 (announcing this seesaw's
//      ID and the new direction) to the Pi audio player.
//
//   2. Plays the chase animation on both LED strips simultaneously,
//      forward for SIDE_A and reverse for SIDE_B. A new tilt event
//      interrupts an in-progress chase, after a short cooldown so very
//      fast bounces don't keep stomping on the chase.
//
// Hardware:
//   - Teensy 4.0 powered from per-seesaw 24V to 5V buck on VIN.
//   - MPU6050 breakout on the default Wire bus (pins 18 SDA / 19 SCL).
//   - WS2813 strips driven via a 74AHCT125 (5V) buffer.
//   - MAX3485 (3.3V) RS485 transceiver on Serial1 with DE/RE on
//     PIN_RS485_DE; bus carries A, B and a GND reference wire.
//
// Per-board configuration: edit SEESAW_ID in config.h before flashing.
// Gyro axis, sampling rate, velocity threshold, and event cooldown also
// live in config.h.
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
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

// ---- Gyro / tilt state ---------------------------------------------

Adafruit_MPU6050 mpu;
bool           mpuOk = false;

enum MotionDir { MOTION_NONE, MOTION_TOWARD_A, MOTION_TOWARD_B };
MotionDir      motionDir = MOTION_NONE;
elapsedMillis  sampleTimer;
elapsedMillis  cooldownTimer;

// ---- Chase playback -------------------------------------------------

bool           chaseActive = false;
int            chaseFrame  = 0;
int            chaseStep   = 1;        // +1 forward, -1 reverse
elapsedMillis  frameTimer;
const uint16_t FRAME_INTERVAL_MS = 1000 / CHASE_FPS;

// ---- RS485 ----------------------------------------------------------

uint8_t        txSeq = 0;

// ---- Forward decls --------------------------------------------------

static float   readGyroAxis();
static void    pollTilt();
static void    onTiltChange(uint8_t direction);
static void    sendEvent(uint8_t direction);
static void    startChase(uint8_t direction);
static void    tickChase();
static void    drawFrame(int frameIndex);
static void    clearStrips();
static void    showStrips();

// ---- Setup / loop ---------------------------------------------------

void setup() {
  Serial1.begin(RS485_BAUD);
  Serial1.transmitterEnable(PIN_RS485_DE);

  leds1.begin();
  leds2.begin();
  clearStrips();
  showStrips();

  Wire.begin();
  Wire.setClock(400000);
  mpuOk = mpu.begin(MPU_I2C_ADDR, &Wire);
  if (mpuOk) {
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }

  // Reversal-based detection only fires on a true direction change, so
  // a tilted-at-power-up seesaw never produces a spurious chase. The
  // first event will fire when someone actually rocks the seesaw and
  // it bottoms out.
  motionDir = MOTION_NONE;
  cooldownTimer = TILT_EVENT_COOLDOWN_MS;   // start "expired"

  randomSeed(analogRead(A0) ^ micros());
}

void loop() {
  pollTilt();
  tickChase();
}

// ---- Tilt detection (gyro reversal) --------------------------------

static float readGyroAxis() {
  if (!mpuOk) return 0.0f;
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  float v = 0.0f;
  switch (TILT_GYRO_AXIS) {
    case TILT_GYRO_AXIS_X: v = g.gyro.x; break;
    case TILT_GYRO_AXIS_Y: v = g.gyro.y; break;
    case TILT_GYRO_AXIS_Z: v = g.gyro.z; break;
  }
  v *= 180.0f / PI;                        // rad/s -> deg/s
  return TILT_INVERT ? -v : v;
}

static void pollTilt() {
  if (!mpuOk) return;
  if (sampleTimer < TILT_SAMPLE_INTERVAL_MS) return;
  sampleTimer = 0;

  float vel = readGyroAxis();

  // Negative velocity = moving toward SIDE_A, positive = toward SIDE_B.
  // Inside the +/- TILT_MIN_VELOCITY_DPS dead zone we hold the previous
  // direction so noise around zero cannot fake a reversal.
  if (vel <= -TILT_MIN_VELOCITY_DPS) {
    if (motionDir == MOTION_TOWARD_B
        && cooldownTimer >= TILT_EVENT_COOLDOWN_MS) {
      // Reversal at the SIDE_B peak: side B just bottomed out.
      cooldownTimer = 0;
      onTiltChange(DIR_B);
    }
    motionDir = MOTION_TOWARD_A;
  } else if (vel >= TILT_MIN_VELOCITY_DPS) {
    if (motionDir == MOTION_TOWARD_A
        && cooldownTimer >= TILT_EVENT_COOLDOWN_MS) {
      // Reversal at the SIDE_A peak: side A just bottomed out.
      cooldownTimer = 0;
      onTiltChange(DIR_A);
    }
    motionDir = MOTION_TOWARD_B;
  }
  // else: in dead zone, motionDir is held.
}

static void onTiltChange(uint8_t direction) {
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

static inline uint8_t scaleBrightness(uint8_t v) {
  return (uint8_t)(((uint16_t)v * (uint16_t)LED_BRIGHTNESS) / 255);
}

static void drawFrame(int frameIndex) {
  for (int i = 0; i < (int)CHASE_NUM_LEDS; i++) {
    uint8_t r = scaleBrightness(pgm_read_byte(&chase[frameIndex][i * 3 + 0]));
    uint8_t g = scaleBrightness(pgm_read_byte(&chase[frameIndex][i * 3 + 1]));
    uint8_t b = scaleBrightness(pgm_read_byte(&chase[frameIndex][i * 3 + 2]));
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
