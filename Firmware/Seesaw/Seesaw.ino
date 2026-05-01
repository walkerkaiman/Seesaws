// =====================================================================
// Seesaw firmware (Teensy 4.0)
// =====================================================================
//
// An MPU6050 measures the seesaw's angular velocity on the configured
// rotation axis. Events fire on direction reversal - the moment one side
// reaches its lowest point and starts coming back up. This is the impact
// moment, and it works at any amplitude (small kids and adults trigger
// the same way).
//
// The firmware runs in one of two modes at any time:
//
//   IDLE: continuously loops the idle animation in idle.h on all four
//         LED strips, with a configurable per-strip frame offset so
//         the strips animate phase-shifted. Entered on boot, and re-
//         entered automatically after IDLE_TIMEOUT_MS without a tilt
//         event.
//
//   PLAY: triggered by a tilt event. On every event the firmware:
//         1. Sends a 6-byte event frame over RS485 (announcing this
//            seesaw's ID and the new direction) to the Pi audio player.
//         2. Plays the chase animation on the pair of LED strips that
//            lives on whichever side just bottomed out. DIR_A lights
//            the SIDE_A pair (PIN_LED_STRIP_A1/A2) playing the chase
//            forward; DIR_B lights the SIDE_B pair (PIN_LED_STRIP_B1/B2)
//            playing it in reverse. The pair on the other side stays
//            dark for the duration of the chase, so the visual
//            feedback localizes to the side that just hit the ground.
//         A new tilt event interrupts an in-progress chase (including
//         swapping which pair is lit if the new event is on the
//         opposite side), after a short cooldown so very fast bounces
//         don't keep stomping on the chase.
//
// In addition to tilt events, the firmware sends a state-change event
// (EVT_STATE_IDLE / EVT_STATE_PLAY) on every IDLE<->PLAY transition,
// including the boot-into-IDLE transition. The Pi audio player has a
// listener stub for these but does nothing with them today; the wire
// path is in place so idle-aware audio behavior (attract music,
// prompts, etc.) can be added later without changing this firmware.
//
// Hardware:
//   - Teensy 4.0 powered from per-seesaw 24V to 5V buck on VIN.
//   - MPU6050 breakout on the default Wire bus (pins 18 SDA / 19 SCL).
//   - Four WS2813 strips (45 LEDs each) driven via a 74AHCT125 (5V)
//     buffer, two per side of the seesaw.
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
#include "idle.h"

static_assert(CHASE_NUM_LEDS <= STRIP_NUM_LEDS,
              "CHASE_NUM_LEDS must not exceed STRIP_NUM_LEDS");
static_assert(IDLE_NUM_LEDS <= STRIP_NUM_LEDS,
              "IDLE_NUM_LEDS must not exceed STRIP_NUM_LEDS");

// ---- LED strips -----------------------------------------------------
//
// Four physical strips, paired by which side of the seesaw they live on.
// Pair A = ledsA1 + ledsA2 on SIDE_A; pair B = ledsB1 + ledsB2 on SIDE_B.
// Only one pair lights at a time during a chase (the one matching the
// triggered side); both strips in that pair receive identical pixel
// data so they stay perfectly in sync. Buffers are sized to
// STRIP_NUM_LEDS (the physical strip length); drawFrame only writes the
// first CHASE_NUM_LEDS pixels, so a chase narrower than the strip
// leaves the trailing pixels dark (cleared on stop).

byte           ledA1Drawing[STRIP_NUM_LEDS * 3];
DMAMEM byte    ledA1Display[STRIP_NUM_LEDS * 12];
WS2812Serial   ledsA1(STRIP_NUM_LEDS, ledA1Display, ledA1Drawing,
                      PIN_LED_STRIP_A1, WS2811_GRB);

byte           ledA2Drawing[STRIP_NUM_LEDS * 3];
DMAMEM byte    ledA2Display[STRIP_NUM_LEDS * 12];
WS2812Serial   ledsA2(STRIP_NUM_LEDS, ledA2Display, ledA2Drawing,
                      PIN_LED_STRIP_A2, WS2811_GRB);

byte           ledB1Drawing[STRIP_NUM_LEDS * 3];
DMAMEM byte    ledB1Display[STRIP_NUM_LEDS * 12];
WS2812Serial   ledsB1(STRIP_NUM_LEDS, ledB1Display, ledB1Drawing,
                      PIN_LED_STRIP_B1, WS2811_GRB);

byte           ledB2Drawing[STRIP_NUM_LEDS * 3];
DMAMEM byte    ledB2Display[STRIP_NUM_LEDS * 12];
WS2812Serial   ledsB2(STRIP_NUM_LEDS, ledB2Display, ledB2Drawing,
                      PIN_LED_STRIP_B2, WS2811_GRB);

WS2812Serial* const stripsA[]   = { &ledsA1, &ledsA2 };  // SIDE_A pair
WS2812Serial* const stripsB[]   = { &ledsB1, &ledsB2 };  // SIDE_B pair
WS2812Serial* const allStrips[] = { &ledsA1, &ledsA2, &ledsB1, &ledsB2 };
const size_t   STRIPS_PER_SIDE = sizeof(stripsA)   / sizeof(stripsA[0]);
const size_t   NUM_STRIPS      = sizeof(allStrips) / sizeof(allStrips[0]);

// ---- Gyro / tilt state ---------------------------------------------

Adafruit_MPU6050 mpu;
bool           mpuOk = false;

enum MotionDir { MOTION_NONE, MOTION_TOWARD_A, MOTION_TOWARD_B };
MotionDir      motionDir = MOTION_NONE;
elapsedMillis  sampleTimer;
elapsedMillis  cooldownTimer;

// ---- System state machine ------------------------------------------
//
// Tracks whether the firmware is running the play chase or the looping
// idle animation. idleTimer counts ms since the last tilt event; when
// it crosses IDLE_TIMEOUT_MS in PLAY mode and no chase is currently
// running, the system drops back into IDLE.

enum SystemState { STATE_IDLE, STATE_PLAY };
SystemState    systemState = STATE_IDLE;
elapsedMillis  idleTimer;        // ms since the last tilt event

// ---- Chase playback (PLAY mode) ------------------------------------

enum ChaseSide { CHASE_ON_SIDE_A, CHASE_ON_SIDE_B };

bool           chaseActive = false;
int            chaseFrame  = 0;
int            chaseStep   = 1;                // +1 forward, -1 reverse
ChaseSide      chaseSide   = CHASE_ON_SIDE_A;  // which pair is currently lit
elapsedMillis  frameTimer;
const uint16_t FRAME_INTERVAL_MS = 1000 / CHASE_FPS;

// ---- Idle animation (IDLE mode) ------------------------------------

int            idleFrame = 0;                  // "lead" frame; per-strip
                                               // frames are this + offset
elapsedMillis  idleFrameTimer;
const uint16_t IDLE_FRAME_INTERVAL_MS = 1000 / IDLE_FPS;

const int      IDLE_FRAME_OFFSETS[] = {
                 IDLE_FRAME_OFFSET_A1,
                 IDLE_FRAME_OFFSET_A2,
                 IDLE_FRAME_OFFSET_B1,
                 IDLE_FRAME_OFFSET_B2,
               };
static_assert(sizeof(IDLE_FRAME_OFFSETS) / sizeof(IDLE_FRAME_OFFSETS[0])
              == sizeof(allStrips) / sizeof(allStrips[0]),
              "IDLE_FRAME_OFFSETS must have one entry per strip");

// ---- RS485 ----------------------------------------------------------

uint8_t        txSeq = 0;

// ---- Forward decls --------------------------------------------------

static float   readGyroAxis();
static void    pollTilt();
static void    onTiltChange(uint8_t direction);
static void    sendEvent(uint8_t event);
static void    enterIdleState();
static void    enterPlayState();
static void    startChase(uint8_t direction);
static void    tickChase();
static void    tickIdle();
static void    drawFrame(int frameIndex);
static void    drawIdleFrame();
static void    clearStrips();
static void    showStrips();

// ---- Setup / loop ---------------------------------------------------

void setup() {
  Serial1.begin(RS485_BAUD);
  Serial1.transmitterEnable(PIN_RS485_DE);

  for (size_t s = 0; s < NUM_STRIPS; s++) allStrips[s]->begin();
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

  // Seed before entering IDLE so the EVT_STATE_IDLE boot frame's
  // resend jitter is randomized too.
  randomSeed(analogRead(A0) ^ micros());

  // Boot directly into IDLE so the seesaw shows the idle animation
  // immediately on power-up. enterIdleState() also emits the initial
  // EVT_STATE_IDLE frame so the Pi (if listening) sees the seesaw
  // come up. The first tilt event flips this to PLAY.
  enterIdleState();
}

void loop() {
  pollTilt();

  if (systemState == STATE_PLAY) {
    tickChase();
    // After a play chase finishes and IDLE_TIMEOUT_MS has passed since
    // the last tilt, drop back into IDLE. Don't yank the seesaw out of
    // PLAY mid-chase even if the timeout expires.
    if (!chaseActive && idleTimer >= IDLE_TIMEOUT_MS) {
      enterIdleState();
    }
  } else {
    tickIdle();
  }
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
  // Any tilt event resets the idle countdown, regardless of state.
  idleTimer = 0;
  // Order matters on the wire: if this tilt is what lifts us out of
  // IDLE, the EVT_STATE_PLAY frame goes out first (from enterPlayState),
  // so the Pi sees the state transition before the tilt that caused it.
  if (systemState == STATE_IDLE) {
    enterPlayState();
  }
  sendEvent(direction);
  startChase(direction);
}

// ---- State transitions ---------------------------------------------
//
// Both transition functions emit a state-change event on RS485 after
// updating local state. The Pi has a listener stub for these (see
// Audio/seesaw_audio.py on_state_change); today it just logs and does
// nothing else, but the firmware always sends them so the wire path
// is in place.

static void enterIdleState() {
  systemState = STATE_IDLE;
  idleFrame = 0;
  idleFrameTimer = 0;
  // tickIdle() will overwrite all pixels on its first call below; we
  // don't need to clear here. Strips may already be dark from a
  // chase-end clear, which is fine.
  drawIdleFrame();
  sendEvent(EVT_STATE_IDLE);
}

static void enterPlayState() {
  systemState = STATE_PLAY;
  // Wipe any in-progress idle animation so the new chase starts from
  // a clean slate. The chase's drawFrame() handles black-fill of the
  // non-active pair on every frame, but the idle animation may have
  // painted pixels past CHASE_NUM_LEDS or on the active pair too;
  // clearStrips() handles both at once.
  clearStrips();
  sendEvent(EVT_STATE_PLAY);
}

// ---- RS485 transmit -------------------------------------------------
//
// sendEvent() handles every kind of frame the firmware emits: tilt
// events (DIR_A / DIR_B) from onTiltChange and state-change events
// (EVT_STATE_*) from the transition helpers. Each call blocks for
// roughly RS485_RESEND_COUNT * RS485_RESEND_JITTER_MAX_MS in the worst
// case (Serial1.flush is blocking and the jitter delay is too), so
// this is intentionally only invoked at event boundaries, never every
// loop iteration.

static void sendEvent(uint8_t event) {
  uint8_t seq = txSeq++;
  uint8_t buf[FRAME_SIZE];
  buildFrame(buf, SEESAW_ID, event, seq);
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
    chaseSide  = CHASE_ON_SIDE_A;
  } else {
    chaseFrame = CHASE_NUM_FRAMES - 1;
    chaseStep  = -1;
    chaseSide  = CHASE_ON_SIDE_B;
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
  // Active pair gets the chase frame; the other pair is held dark for
  // the duration of the chase so feedback localizes to the triggered
  // side. The inactive pair is blanked here on every frame so a fresh
  // chase that fires on the opposite side immediately darkens the
  // previously-lit pair without needing a separate clear step.
  WS2812Serial* const* active   = (chaseSide == CHASE_ON_SIDE_A) ? stripsA : stripsB;
  WS2812Serial* const* inactive = (chaseSide == CHASE_ON_SIDE_A) ? stripsB : stripsA;

  for (int i = 0; i < (int)CHASE_NUM_LEDS; i++) {
    uint8_t r = scaleBrightness(pgm_read_byte(&chase[frameIndex][i * 3 + 0]));
    uint8_t g = scaleBrightness(pgm_read_byte(&chase[frameIndex][i * 3 + 1]));
    uint8_t b = scaleBrightness(pgm_read_byte(&chase[frameIndex][i * 3 + 2]));
    for (size_t s = 0; s < STRIPS_PER_SIDE; s++) {
      active[s]->setPixel(i, r, g, b);
      inactive[s]->setPixel(i, 0, 0, 0);
    }
  }
  showStrips();
}

// ---- Idle animation (IDLE mode) ------------------------------------

static void tickIdle() {
  if (idleFrameTimer < IDLE_FRAME_INTERVAL_MS) return;
  idleFrameTimer -= IDLE_FRAME_INTERVAL_MS;

  idleFrame = (idleFrame + 1) % (int)IDLE_NUM_FRAMES;
  drawIdleFrame();
}

static void drawIdleFrame() {
  // Each strip plays the same idle animation but offset by its own
  // IDLE_FRAME_OFFSETS[s] frames, so the four strips are phase-shifted.
  // The animation loops indefinitely while the system is in IDLE.
  for (size_t s = 0; s < NUM_STRIPS; s++) {
    int frame = (idleFrame + IDLE_FRAME_OFFSETS[s]) % (int)IDLE_NUM_FRAMES;
    if (frame < 0) frame += (int)IDLE_NUM_FRAMES;   // safety if offset is negative
    for (int i = 0; i < (int)IDLE_NUM_LEDS; i++) {
      uint8_t r = scaleBrightness(pgm_read_byte(&idle[frame][i * 3 + 0]));
      uint8_t g = scaleBrightness(pgm_read_byte(&idle[frame][i * 3 + 1]));
      uint8_t b = scaleBrightness(pgm_read_byte(&idle[frame][i * 3 + 2]));
      allStrips[s]->setPixel(i, r, g, b);
    }
  }
  showStrips();
}

static void clearStrips() {
  for (int i = 0; i < (int)STRIP_NUM_LEDS; i++) {
    for (size_t s = 0; s < NUM_STRIPS; s++) {
      allStrips[s]->setPixel(i, 0, 0, 0);
    }
  }
}

static void showStrips() {
  for (size_t s = 0; s < NUM_STRIPS; s++) {
    allStrips[s]->show();
  }
}
