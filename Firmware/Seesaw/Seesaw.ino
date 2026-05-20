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
//   IDLE: continuously samples a procedural grayscale 3D noise field
//         (FastLED inoise8, see idle_noise.h) on all four LED strips.
//         The noise z axis advances slowly each frame, so the idle
//         animation runs indefinitely without a visible loop. Entered
//         on boot and re-entered automatically after IDLE_TIMEOUT_MS
//         without a tilt event.
//
//   PLAY: triggered by a tilt event. On every event the firmware:
//         1. Sends a 6-byte event frame over RS485 (announcing this
//            seesaw's ID and the new direction) to the central audio Teensy.
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
// including the boot-into-IDLE transition. The central audio Teensy has a
// listener stub for these but does nothing with them today; the wire
// path is in place so idle-aware audio behavior (attract music,
// prompts, etc.) can be added later without changing this firmware.
//
// Hardware:
//   - Teensy 4.0 powered from per-seesaw 24V to 5V buck on VIN.
//   - MPU6050 breakout on the default Wire bus (pins 18 SDA / 19 SCL).
//   - Four WS2813 strips (45 LEDs each) on GPIO 6/7/8/9 via a 74AHCT125
//     (5V) buffer, two per side of the seesaw (Adafruit_NeoPixel driver).
//   - MAX3485 (3.3V) on Serial1: RX pin 0, TX pin 1, DE+RE pin 2.
//
// Per-board configuration: edit SEESAW_ID in config.h before flashing.
// Gyro axis, sampling rate, velocity threshold, and event cooldown also
// live in config.h.
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_NeoPixel.h>

#include "config.h"
#include "protocol.h"
#include "chase.h"
#include "idle_noise.h"

static_assert(CHASE_NUM_LEDS <= STRIP_NUM_LEDS,
              "CHASE_NUM_LEDS must not exceed STRIP_NUM_LEDS");

// ---- LED strips (Adafruit_NeoPixel on fixed GPIO 6/7/8/9) ------------
//
// Thin wrapper keeps the rest of the sketch unchanged. NeoPixel can use
// any GPIO (unlike WS2812Serial, which requires UART TX pins).

class LedStrip {
public:
  Adafruit_NeoPixel pixels;

  LedStrip(uint16_t count, uint8_t pin)
      : pixels(count, pin, NEO_GRB + NEO_KHZ800) {}

  void begin() {
    pixels.begin();
    pixels.show();
  }

  void setPixel(int i, uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(i, pixels.Color(r, g, b));
  }

  void show() { pixels.show(); }
};

LedStrip ledsA1(STRIP_NUM_LEDS, PIN_LED_STRIP_A1);
LedStrip ledsA2(STRIP_NUM_LEDS, PIN_LED_STRIP_A2);
LedStrip ledsB1(STRIP_NUM_LEDS, PIN_LED_STRIP_B1);
LedStrip ledsB2(STRIP_NUM_LEDS, PIN_LED_STRIP_B2);

LedStrip* const stripsA[]   = { &ledsA1, &ledsA2 };
LedStrip* const stripsB[]   = { &ledsB1, &ledsB2 };
LedStrip* const allStrips[] = { &ledsA1, &ledsA2, &ledsB1, &ledsB2 };
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
//
// Procedural grayscale noise (see idle_noise.h). idleNoiseZ is the
// shared time coordinate of the 3D noise field; it advances by
// IDLE_NOISE_Z_STEP every idle tick (IDLE_FPS Hz). uint16_t wrap is
// fine - inoise8 is continuous across the boundary.

uint16_t       idleNoiseZ = 0;
elapsedMillis  idleFrameTimer;
const uint16_t IDLE_FRAME_INTERVAL_MS = 1000 / IDLE_FPS;

// ---- RS485 ----------------------------------------------------------

uint8_t        txSeq = 0;

// ---- Diagnostics (USB Serial) ----------------------------------------

#if SERIAL_DIAG_ENABLE
static elapsedMillis diagTimer;
static uint32_t      diagIdleDraws   = 0;
static uint32_t      diagChaseDraws    = 0;
static uint32_t      diagShowCalls     = 0;
static uint32_t      diagTiltEvents    = 0;
static uint32_t      diagRs485Events   = 0;
static float         diagLastGyroDps   = 0.0f;

static const char* motionDirName(MotionDir d) {
  switch (d) {
    case MOTION_TOWARD_A: return "TOWARD_A";
    case MOTION_TOWARD_B: return "TOWARD_B";
    default:              return "NONE";
  }
}

static const char* systemStateName(SystemState s) {
  return (s == STATE_IDLE) ? "IDLE" : "PLAY";
}

static void diagPrintPinWarnings() {
  if (PIN_RS485_DE == 1) {
    Serial.println("  WARN: PIN_RS485_DE=1 conflicts with Serial1 TX");
  }
  const uint8_t ledPins[] = {
    PIN_LED_STRIP_A1, PIN_LED_STRIP_A2,
    PIN_LED_STRIP_B1, PIN_LED_STRIP_B2,
  };
  if (PIN_RS485_DE == ledPins[0] || PIN_RS485_DE == ledPins[1]
      || PIN_RS485_DE == ledPins[2] || PIN_RS485_DE == ledPins[3]) {
    Serial.println("  WARN: PIN_RS485_DE overlaps an LED data pin");
  }
}

static void diagPrintBootBanner() {
  Serial.println();
  Serial.println("=== Seesaw diag ===");
  Serial.print("SEESAW_ID=");
  Serial.println(SEESAW_ID);
  Serial.print("state=");
  Serial.println(systemStateName(systemState));
  Serial.print("LED pins A1/A2/B1/B2 = ");
  Serial.print(PIN_LED_STRIP_A1); Serial.print('/');
  Serial.print(PIN_LED_STRIP_A2); Serial.print('/');
  Serial.print(PIN_LED_STRIP_B1); Serial.print('/');
  Serial.println(PIN_LED_STRIP_B2);
  Serial.print("STRIP_NUM_LEDS=");
  Serial.print(STRIP_NUM_LEDS);
  Serial.print(" CHASE=");
  Serial.print(CHASE_NUM_LEDS);
  Serial.print('x');
  Serial.print(CHASE_NUM_FRAMES);
  Serial.print(" LED_BRIGHTNESS=");
  Serial.println(LED_BRIGHTNESS);
  if (CHASE_NUM_LEDS < STRIP_NUM_LEDS) {
    Serial.print("  NOTE: chase ");
    Serial.print(CHASE_NUM_LEDS);
    Serial.print(" LEDs tiled across ");
    Serial.print(STRIP_NUM_LEDS);
    Serial.println("-pixel strips");
  }
  Serial.print("idle: noise (inoise8) @ ");
  Serial.print(IDLE_FPS);
  Serial.print(" FPS, z_step=");
  Serial.print(IDLE_NOISE_Z_STEP);
  Serial.print(" x_scale=");
  Serial.print(IDLE_NOISE_X_SCALE);
  Serial.print(" y_stride=");
  Serial.println(IDLE_NOISE_Y_STRIDE);
  Serial.print("MPU6050 @ 0x");
  Serial.print(MPU_I2C_ADDR, HEX);
  Serial.print(": ");
  Serial.println(mpuOk ? "OK" : "FAILED (tilt events disabled)");
  if (!mpuOk) {
    Serial.println("  Check I2C wiring (SDA=18, SCL=19), 3V3 power, ADDR pin.");
  }
  diagPrintPinWarnings();
  Serial.print("After tilt: PLAY until ");
  Serial.print(IDLE_TIMEOUT_MS);
  Serial.println(" ms quiet, then noise idle resumes.");
  Serial.println("===================");
}

static void diagPrintPeriodic() {
  Serial.println();
  Serial.println("--- status ---");
  Serial.print("state=");
  Serial.print(systemStateName(systemState));
  Serial.print(" chaseActive=");
  Serial.print(chaseActive ? 1 : 0);
  Serial.print(" chaseFrame=");
  Serial.print(chaseFrame);
  Serial.print(" noiseZ=");
  Serial.println(idleNoiseZ);

  Serial.print("draws: idle=");
  Serial.print(diagIdleDraws);
  Serial.print(" chase=");
  Serial.print(diagChaseDraws);
  Serial.print(" show=");
  Serial.println(diagShowCalls);
  if (diagIdleDraws == 0 && systemState == STATE_IDLE) {
    Serial.println("  WARN: no idle draws yet - tickIdle/drawIdleNoise not running?");
  }
  if (diagShowCalls == 0) {
    Serial.println("  WARN: showStrips() never called - LEDs cannot update");
  }

  Serial.print("tiltEvents=");
  Serial.print(diagTiltEvents);
  Serial.print(" rs485Events=");
  Serial.println(diagRs485Events);

  if (mpuOk) {
    Serial.print("gyro=");
    Serial.print(diagLastGyroDps, 1);
    Serial.print(" dps motion=");
    Serial.print(motionDirName(motionDir));
    Serial.print(" cooldownMs=");
    Serial.println((unsigned)cooldownTimer);
  } else {
    Serial.println("gyro=N/A (MPU not initialized)");
  }

  Serial.print("idleTimer=");
  Serial.print((unsigned long)idleTimer);
  Serial.print(" ms (timeout ");
  Serial.print(IDLE_TIMEOUT_MS);
  Serial.println(")");
  Serial.println("--------------");
}

static void diagTick() {
  if (diagTimer < SERIAL_DIAG_INTERVAL_MS) return;
  diagTimer = 0;
  diagPrintPeriodic();
}
#endif  // SERIAL_DIAG_ENABLE

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
static void    drawIdleNoise();
static void    resetIdleNoise();
static void    clearStrips();
static void    showStrips();
#if BENCH_LED_SELFTEST
static void    benchLedSelfTest();
#endif

// ---- Setup / loop ---------------------------------------------------

#if SERIAL_DIAG_ENABLE
static void diagCheckpoint(const char *label) {
  Serial.println(label);
  Serial.flush();
}
#else
static void diagCheckpoint(const char *) {}
#endif

void setup() {
#if SERIAL_DIAG_ENABLE
  Serial.begin(SERIAL_BAUD);
  while (!Serial && millis() < 3000) { /* USB console */ }
  static uint32_t bootCount = 0;
  bootCount++;
  Serial.println();
  Serial.print("=== Seesaw setup #");
  Serial.print(bootCount);
  Serial.println(bootCount > 1 ? " (reboot loop?)" : "");
  Serial.flush();
#endif

  diagCheckpoint("Serial1 + RS485 DE...");
  Serial1.begin(RS485_BAUD);
  Serial1.transmitterEnable(PIN_RS485_DE);

  diagCheckpoint("LED begin...");
  for (size_t s = 0; s < NUM_STRIPS; s++) {
    allStrips[s]->begin();
#if SERIAL_DIAG_ENABLE
    Serial.print("  strip ");
    Serial.println(s);
    Serial.flush();
#endif
  }

  diagCheckpoint("clearStrips...");
  clearStrips();

  diagCheckpoint("showStrips (per strip)...");
  for (size_t s = 0; s < NUM_STRIPS; s++) {
#if SERIAL_DIAG_ENABLE
    Serial.print("  show strip ");
    Serial.println(s);
    Serial.flush();
#endif
    allStrips[s]->show();
  }
  diagCheckpoint("showStrips done");

#if BENCH_LED_SELFTEST
  benchLedSelfTest();
#endif

  diagCheckpoint("MPU6050 I2C...");
  Wire.begin();
  Wire.setClock(400000);
  mpuOk = mpu.begin(MPU_I2C_ADDR, &Wire);
  if (mpuOk) {
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }
#if SERIAL_DIAG_ENABLE
  Serial.print("  MPU: ");
  Serial.println(mpuOk ? "OK" : "FAILED");
  Serial.flush();
#endif

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
  // EVT_STATE_IDLE frame so the audio node (if listening) sees the seesaw
  // come up. The first tilt event flips this to PLAY.
  diagCheckpoint("enterIdleState...");
  enterIdleState();
  diagCheckpoint("setup complete");

#if SERIAL_DIAG_ENABLE
  diagPrintBootBanner();
  diagTimer = 0;
#endif
}

void loop() {
  pollTilt();

#if SERIAL_DIAG_ENABLE
  diagTick();
#endif

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
#if SERIAL_DIAG_ENABLE
  diagLastGyroDps = vel;
#endif

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
#if SERIAL_DIAG_ENABLE
  diagTiltEvents++;
  Serial.print("TILT dir=");
  Serial.print(direction == DIR_A ? 'A' : 'B');
  Serial.print(" state was ");
  Serial.println(systemStateName(systemState));
#endif
  // Any tilt event resets the idle countdown, regardless of state.
  idleTimer = 0;
  // Order matters on the wire: if this tilt is what lifts us out of
  // IDLE, the EVT_STATE_PLAY frame goes out first (from enterPlayState),
  // so the audio node sees the state transition before the tilt that caused it.
  if (systemState == STATE_IDLE) {
    enterPlayState();
  }
  sendEvent(direction);
  startChase(direction);
}

// ---- State transitions ---------------------------------------------
//
// Both transition functions emit a state-change event on RS485 after
// updating local state. The audio Teensy has a listener stub for these (see
// Audio/SeesawAudio onStateChange); today it just logs and does
// nothing else, but the firmware always sends them so the wire path
// is in place.

static void enterIdleState() {
#if SERIAL_DIAG_ENABLE
  Serial.println("-> enterIdleState");
#endif
  systemState = STATE_IDLE;
  idleFrameTimer = 0;
  // idleNoiseZ is intentionally NOT reset here, so the noise field
  // appears continuous across PLAY -> IDLE transitions. Call
  // resetIdleNoise() instead if you want each idle session to start
  // from a fixed point in the noise.
  drawIdleNoise();
  sendEvent(EVT_STATE_IDLE);
}

static void enterPlayState() {
#if SERIAL_DIAG_ENABLE
  Serial.println("-> enterPlayState");
#endif
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
#if SERIAL_DIAG_ENABLE
  diagRs485Events++;
  Serial.print("RS485 event=");
  Serial.print(event);
  Serial.print(" seq=");
  Serial.println(txSeq);
#endif
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
#if SERIAL_DIAG_ENABLE
  diagChaseDraws++;
#endif
  // Active pair gets the chase frame; the other pair is held dark for
  // the duration of the chase so feedback localizes to the triggered
  // side. The inactive pair is blanked here on every frame so a fresh
  // chase that fires on the opposite side immediately darkens the
  // previously-lit pair without needing a separate clear step.
  LedStrip* const* active   = (chaseSide == CHASE_ON_SIDE_A) ? stripsA : stripsB;
  LedStrip* const* inactive = (chaseSide == CHASE_ON_SIDE_A) ? stripsB : stripsA;

  for (int i = 0; i < (int)STRIP_NUM_LEDS; i++) {
    const int src = i % (int)CHASE_NUM_LEDS;
    uint8_t r = scaleBrightness(pgm_read_byte(&chase[frameIndex][src * 3 + 0]));
    uint8_t g = scaleBrightness(pgm_read_byte(&chase[frameIndex][src * 3 + 1]));
    uint8_t b = scaleBrightness(pgm_read_byte(&chase[frameIndex][src * 3 + 2]));
    for (size_t s = 0; s < STRIPS_PER_SIDE; s++) {
      active[s]->setPixel(i, r, g, b);
      inactive[s]->setPixel(i, 0, 0, 0);
    }
  }
  showStrips();
}

// ---- Idle animation (IDLE mode) ------------------------------------
//
// Procedural grayscale 3D noise idle. Every IDLE_FPS tick we advance
// idleNoiseZ by IDLE_NOISE_Z_STEP and resample the full pixel grid;
// see idle_noise.h for the field layout. Output is grayscale
// (r = g = b = noise sample, after LED_BRIGHTNESS scaling) so we get a
// smooth drifting shimmer that never repeats.

static void tickIdle() {
  if (idleFrameTimer < IDLE_FRAME_INTERVAL_MS) return;
  idleFrameTimer -= IDLE_FRAME_INTERVAL_MS;

  idleNoiseZ = (uint16_t)(idleNoiseZ + (uint16_t)IDLE_NOISE_Z_STEP);
  drawIdleNoise();
}

static void drawIdleNoise() {
#if SERIAL_DIAG_ENABLE
  diagIdleDraws++;
#endif
  for (size_t s = 0; s < NUM_STRIPS; s++) {
    for (int i = 0; i < (int)STRIP_NUM_LEDS; i++) {
      uint8_t v = sampleIdleNoise((uint8_t)s, (uint16_t)i, idleNoiseZ);
      v = scaleBrightness(v);
      allStrips[s]->setPixel(i, v, v, v);
    }
  }
  showStrips();
}

// Defined but not called by default. Call from enterIdleState() if you
// want each idle session to begin from a fixed point in the noise field.
__attribute__((unused))
static void resetIdleNoise() {
  idleNoiseZ = 0;
}

static void clearStrips() {
  for (int i = 0; i < (int)STRIP_NUM_LEDS; i++) {
    for (size_t s = 0; s < NUM_STRIPS; s++) {
      allStrips[s]->setPixel(i, 0, 0, 0);
    }
  }
}

#if BENCH_LED_SELFTEST
static void benchLedSelfTest() {
  Serial.println("Bench LED self-test: all strips full white 0.5s");
  Serial.flush();
  const uint8_t v = scaleBrightness(80);
  for (int i = 0; i < (int)STRIP_NUM_LEDS; i++) {
    for (size_t s = 0; s < NUM_STRIPS; s++) {
      allStrips[s]->setPixel(i, v, v, v);
    }
  }
  showStrips();
  delay(500);
  clearStrips();
  showStrips();
}
#endif

static void showStrips() {
#if SERIAL_DIAG_ENABLE
  diagShowCalls++;
#endif
  for (size_t s = 0; s < NUM_STRIPS; s++) {
    allStrips[s]->show();
  }
}
