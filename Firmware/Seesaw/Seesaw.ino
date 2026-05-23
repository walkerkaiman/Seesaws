// =====================================================================
// Seesaw firmware (Teensy 4.0)
// =====================================================================
//
// An MPU6050 measures the seesaw's tilt along its length axis using
// the accelerometer. The signal is low-pass filtered, then run through
// a three-zone state machine (NEUTRAL / A_DOWN / B_DOWN) with
// hysteresis on the zone boundaries. Inside an active zone the
// firmware tracks the running peak of the filtered signal and fires
// the trigger only after the signal has backed off that peak by
// TILT_REVERSAL_DELTA_G -- i.e. when the seesaw has actually started
// reversing direction. An "armed-side" flag also enforces alternation:
// once one side fires, the same side cannot fire again until the other
// side fires, regardless of bounce or hand-jitter. See config.h for
// the axis mapping, thresholds, and reversal delta, and pollTilt()
// for the state machine.
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
//         2. Starts that side's LED chase IF that side is not already
//            chasing. DIR_A lights the SIDE_A pair (PIN_LED_STRIP_A1/A2)
//            playing the chase forward; DIR_B lights the SIDE_B pair
//            (PIN_LED_STRIP_B1/B2) playing it in reverse. Each side has
//            its own independent chase pipeline, so a fast rocker can
//            have both sides chasing concurrently. An in-progress chase
//            is NEVER interrupted -- if a side's chase is already
//            running when that side fires again, the bus event still
//            goes out (so audio plays) but the LED chase keeps playing
//            its current frame sequence to completion.
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
// Tilt axis, fire/release thresholds, LPF alpha, and zero offset also
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

// ---- Tilt detection state (MPU6050 accel peak-on-reversal) ---------
//
// The seesaw's tilt-along-length is read from the accelerometer axis
// selected by TILT_AXIS_ACCEL (with TILT_INVERT_ACCEL flipping sign so
// SIDE_A down maps to +g). The signal goes through a single-pole IIR
// LPF; the filtered value drives a three-zone state machine (NEUTRAL /
// A_DOWN / B_DOWN) with TILT_FIRE_THRESHOLD_G to enter a side and
// TILT_RELEASE_THRESHOLD_G to leave it. Inside an active zone we track
// the running peak and fire only once the signal has come back from
// that peak by at least TILT_REVERSAL_DELTA_G -- i.e. the seesaw has
// genuinely started reversing. An armed-side flag (NONE / A / B)
// enforces alternation: the same side never fires twice in a row.

Adafruit_MPU6050 mpu;
bool           mpuOk = false;

enum TiltZone   { ZONE_NEUTRAL, ZONE_A_DOWN, ZONE_B_DOWN };
enum ArmedSide  { ARM_NONE, ARM_A, ARM_B };

static TiltZone  tiltZone         = ZONE_NEUTRAL;
static ArmedSide tiltArmed        = ARM_NONE;
static float     tiltFiltered     = 0.0f;    // LPF-filtered length-axis g
static bool      tiltPrimed       = false;   // first sample seeds the LPF
static float     tiltPeakG        = 0.0f;    // running peak (max in A_DOWN, min in B_DOWN); meaningless in NEUTRAL
static bool      tiltFiredThisZone = false;  // becomes true after we fire in the current excursion
static float     lastAccelX       = 0.0f;    // raw accel for diag, last poll
static float     lastAccelY       = 0.0f;
static float     lastAccelZ       = 0.0f;
static float     lastGyroDps      = 0.0f;    // gyro along pivot, diag only

elapsedMillis  sampleTimer;

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
//
// One independent chase pipeline per side. Each tilt event starts that
// side's chase IF that side isn't already running; an in-flight chase
// is never interrupted. Both sides can run concurrently when the rider
// rocks faster than a single chase plays out.

enum ChaseSide   { CHASE_SIDE_A = 0, CHASE_SIDE_B = 1, CHASE_NUM_SIDES = 2 };

struct ChaseState {
  bool          active;
  int           frame;
  int           step;            // +1 forward, -1 reverse
  elapsedMillis frameTimer;
};
static ChaseState chases[CHASE_NUM_SIDES];

const uint16_t FRAME_INTERVAL_MS = 1000 / CHASE_FPS;

// ---- Idle animation (IDLE mode) ------------------------------------
//
// Procedural grayscale noise (see idle_noise.h). idleNoiseZ is the
// shared time coordinate of the 3D noise field; it advances by
// IDLE_NOISE_Z_STEP every idle tick (IDLE_FPS Hz). uint16_t wrap is
// fine - inoise8 is continuous across the boundary.

uint16_t       idleNoiseZ = 0;
elapsedMillis  idleFrameTimer;
elapsedMillis  idleFadeTimer;       // ms since the most recent enterIdleState()
const uint16_t IDLE_FRAME_INTERVAL_MS = 1000 / IDLE_FPS;

// ---- RS485 ----------------------------------------------------------

uint8_t        txSeq = 0;

// ---- Diagnostics (USB Serial) ----------------------------------------

#if SERIAL_DIAG_ENABLE
static elapsedMillis diagTimer;
static uint32_t      diagIdleDraws    = 0;
static uint32_t      diagChaseDraws   = 0;
static uint32_t      diagShowCalls    = 0;
static uint32_t      diagTiltEvents   = 0;
static uint32_t      diagRs485Events  = 0;

static const char* tiltZoneName(TiltZone z) {
  switch (z) {
    case ZONE_A_DOWN: return "A_DOWN";
    case ZONE_B_DOWN: return "B_DOWN";
    default:          return "NEUTRAL";
  }
}

static const char* armedSideName(ArmedSide a) {
  switch (a) {
    case ARM_A: return "A";
    case ARM_B: return "B";
    default:    return "NONE";
  }
}

static char accelAxisLetter(int axis) {
  switch (axis) {
    case TILT_AXIS_X: return 'X';
    case TILT_AXIS_Y: return 'Y';
    case TILT_AXIS_Z: return 'Z';
    default:          return '?';
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
  Serial.print("tilt: accel axis=");
  Serial.print(accelAxisLetter(TILT_AXIS_ACCEL));
  Serial.print(TILT_INVERT_ACCEL ? " (inverted)" : "");
  Serial.print(", zone-enter=");
  Serial.print(TILT_FIRE_THRESHOLD_G, 2);
  Serial.print(" g, reversal-delta=");
  Serial.print(TILT_REVERSAL_DELTA_G, 2);
  Serial.print(" g, zone-exit=");
  Serial.print(TILT_RELEASE_THRESHOLD_G, 2);
  Serial.print(" g, lpf_alpha=");
  Serial.print(TILT_LPF_ALPHA_X1000 / 1000.0f, 2);
  Serial.print(", offset=");
  Serial.print(TILT_ZERO_OFFSET_G, 2);
  Serial.println(" g");
  Serial.println("       fires at peak-and-reversal inside zone, alternation enforced");
  Serial.print("       gyro axis=");
  Serial.print(accelAxisLetter(TILT_AXIS_GYRO));
  Serial.println(" (diag only)");
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
  Serial.print(" chase A=");
  Serial.print(chases[CHASE_SIDE_A].active ? chases[CHASE_SIDE_A].frame : -1);
  Serial.print(" B=");
  Serial.print(chases[CHASE_SIDE_B].active ? chases[CHASE_SIDE_B].frame : -1);
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
    Serial.print("accel(g) x=");
    Serial.print(lastAccelX, 2);
    Serial.print(" y=");
    Serial.print(lastAccelY, 2);
    Serial.print(" z=");
    Serial.print(lastAccelZ, 2);
    Serial.print("  filteredG=");
    Serial.print(tiltFiltered, 2);
    Serial.print("  zone=");
    Serial.print(tiltZoneName(tiltZone));
    Serial.print(" peakG=");
    if (tiltZone == ZONE_NEUTRAL) {
      Serial.print("--");
    } else {
      Serial.print(tiltPeakG, 2);
    }
    Serial.print(" fired=");
    Serial.print(tiltFiredThisZone ? 1 : 0);
    Serial.print(" armed=");
    Serial.print(armedSideName(tiltArmed));
    Serial.print("  gyro(dps)=");
    Serial.println(lastGyroDps, 1);
  } else {
    Serial.println("accel/gyro=N/A (MPU not initialized)");
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

static void    pollTilt();
static void    onTiltChange(uint8_t direction);
static void    sendEvent(uint8_t event);
static void    enterIdleState();
static void    enterPlayState();
static void    startChase(uint8_t direction);
static void    tickChase();
static bool    anyChaseActive();
static void    tickIdle();
static void    drawSideFrame(int side, int frameIndex);
static void    clearSidePixels(int side);
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

  // Tilt zone state machine starts in NEUTRAL with both sides armed
  // (ARM_NONE), so a power-up while tilted past threshold still allows
  // the *first* trigger on whichever side resolves first. The LPF is
  // primed lazily on the first sample in pollTilt(). tiltPeakG and
  // tiltFiredThisZone are only meaningful inside A_DOWN / B_DOWN; we
  // re-seed them on each NEUTRAL -> X_DOWN transition.
  tiltZone          = ZONE_NEUTRAL;
  tiltArmed         = ARM_NONE;
  tiltFiltered      = 0.0f;
  tiltPrimed        = false;
  tiltPeakG         = 0.0f;
  tiltFiredThisZone = false;

  for (size_t i = 0; i < CHASE_NUM_SIDES; i++) {
    chases[i].active     = false;
    chases[i].frame      = 0;
    chases[i].step       = 1;
    chases[i].frameTimer = 0;
  }

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
    // After both chases finish AND IDLE_TIMEOUT_MS has passed since
    // the last tilt, drop back into IDLE. Don't yank the seesaw out of
    // PLAY mid-chase, even if the timeout expires while a chase runs.
    if (!anyChaseActive() && idleTimer >= IDLE_TIMEOUT_MS) {
      enterIdleState();
    }
  } else {
    tickIdle();
  }
}

// ---- Tilt detection (accel zones with hysteresis + alternation) ----
//
// pickAxis() reads the configured accel/gyro axis from a sensors_event_t.
// readTiltSample() pulls one MPU sample, fills the diag globals, and
// returns the length-axis acceleration in g (with the configured zero
// offset and inversion already applied).

static float pickAxis(const sensors_event_t &ev, int axis) {
  switch (axis) {
    case TILT_AXIS_X: return ev.acceleration.x;
    case TILT_AXIS_Y: return ev.acceleration.y;
    case TILT_AXIS_Z: return ev.acceleration.z;
    default:          return 0.0f;
  }
}

static float pickGyroAxis(const sensors_event_t &ev, int axis) {
  switch (axis) {
    case TILT_AXIS_X: return ev.gyro.x;
    case TILT_AXIS_Y: return ev.gyro.y;
    case TILT_AXIS_Z: return ev.gyro.z;
    default:          return 0.0f;
  }
}

// Adafruit MPU6050 reports accel in m/s^2; convert to g for thresholds.
static const float ACCEL_MS2_PER_G = 9.80665f;

static float readTiltSample() {
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  // Cache raw accel for periodic diag (g units).
  lastAccelX = a.acceleration.x / ACCEL_MS2_PER_G;
  lastAccelY = a.acceleration.y / ACCEL_MS2_PER_G;
  lastAccelZ = a.acceleration.z / ACCEL_MS2_PER_G;

  // Gyro along pivot, deg/s, diag only.
  lastGyroDps = pickGyroAxis(g, TILT_AXIS_GYRO) * (180.0f / PI);

  float lengthG = pickAxis(a, TILT_AXIS_ACCEL) / ACCEL_MS2_PER_G;
  if (TILT_INVERT_ACCEL) lengthG = -lengthG;
  lengthG -= TILT_ZERO_OFFSET_G;
  return lengthG;
}

static void pollTilt() {
  if (!mpuOk) return;
  if (sampleTimer < TILT_SAMPLE_INTERVAL_MS) return;
  sampleTimer = 0;

  const float sample = readTiltSample();

  // First sample seeds the LPF so we don't ramp up from 0 g over the
  // first few hundred ms (which could spuriously cross thresholds).
  if (!tiltPrimed) {
    tiltFiltered = sample;
    tiltPrimed = true;
  } else {
    const float alpha = TILT_LPF_ALPHA_X1000 / 1000.0f;
    tiltFiltered += alpha * (sample - tiltFiltered);
  }

  // Schmitt-trigger zone transitions. FIRE enters the outer band;
  // RELEASE leaves the inner band, so a small bounce around either
  // boundary cannot rapidly toggle zones.
  TiltZone next = tiltZone;
  if (tiltZone != ZONE_A_DOWN && tiltFiltered > +TILT_FIRE_THRESHOLD_G) {
    next = ZONE_A_DOWN;
  } else if (tiltZone != ZONE_B_DOWN && tiltFiltered < -TILT_FIRE_THRESHOLD_G) {
    next = ZONE_B_DOWN;
  } else if (tiltZone == ZONE_A_DOWN && tiltFiltered < +TILT_RELEASE_THRESHOLD_G) {
    next = ZONE_NEUTRAL;
  } else if (tiltZone == ZONE_B_DOWN && tiltFiltered > -TILT_RELEASE_THRESHOLD_G) {
    next = ZONE_NEUTRAL;
  }

  // On entering an A_DOWN / B_DOWN zone, seed the peak tracker to the
  // current sample and clear the "already fired" latch. The fire itself
  // happens later, the first time the signal backs off the peak by
  // TILT_REVERSAL_DELTA_G -- i.e. when the seesaw actually reverses.
  if (next != tiltZone) {
    tiltZone = next;
    if (tiltZone == ZONE_A_DOWN || tiltZone == ZONE_B_DOWN) {
      tiltPeakG         = tiltFiltered;
      tiltFiredThisZone = false;
    }
    // Returning to NEUTRAL: nothing to fire, peak/fired bookkeeping
    // becomes irrelevant until the next zone entry re-seeds them.
  }

  // Reversal detection inside the active zone. Update the peak first so
  // a sample that simultaneously sets a new peak doesn't trip the fire
  // condition on the same tick.
  if (tiltZone == ZONE_A_DOWN) {
    if (tiltFiltered > tiltPeakG) tiltPeakG = tiltFiltered;
    if (!tiltFiredThisZone
        && (tiltArmed == ARM_NONE || tiltArmed == ARM_A)
        && tiltFiltered < tiltPeakG - TILT_REVERSAL_DELTA_G) {
      tiltFiredThisZone = true;
      tiltArmed = ARM_B;
      onTiltChange(DIR_A);
    }
  } else if (tiltZone == ZONE_B_DOWN) {
    if (tiltFiltered < tiltPeakG) tiltPeakG = tiltFiltered;
    if (!tiltFiredThisZone
        && (tiltArmed == ARM_NONE || tiltArmed == ARM_B)
        && tiltFiltered > tiltPeakG + TILT_REVERSAL_DELTA_G) {
      tiltFiredThisZone = true;
      tiltArmed = ARM_A;
      onTiltChange(DIR_B);
    }
  }
  // ZONE_NEUTRAL: no fires; we just wait for the next zone entry.
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
  // idleFadeTimer drives a 0 -> 100% brightness ramp over
  // IDLE_FADE_IN_MS in drawIdleNoise(); reset here so every IDLE entry
  // (boot, PLAY -> IDLE) fades in from black instead of snapping on.
  // idleNoiseZ is intentionally NOT reset, so the noise field appears
  // continuous across PLAY -> IDLE transitions. Call resetIdleNoise()
  // instead if you want each idle session to start from a fixed point
  // in the noise.
  idleFadeTimer = 0;
  drawIdleNoise();
  sendEvent(EVT_STATE_IDLE);
}

static void enterPlayState() {
#if SERIAL_DIAG_ENABLE
  Serial.println("-> enterPlayState");
#endif
  systemState = STATE_PLAY;
  // Wipe any in-progress idle animation so each side's chase starts
  // from a clean slate. drawSideFrame() only writes its own side's
  // pixels (intentionally, so concurrent chases coexist), so we need
  // to zero the other side's pixels and any pixels past CHASE_NUM_LEDS
  // before the first chase frame draws.
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

// ---- Chase playback (per side) -------------------------------------
//
// One ChaseState per physical side. Each tilt event starts that side's
// chase ONLY if it isn't already running (do not interrupt). Both sides
// can run concurrently when the rider rocks faster than a single chase
// plays out, and each side draws only into its own pair of strips.

static LedStrip* const* sideStrips(int side) {
  return (side == CHASE_SIDE_A) ? stripsA : stripsB;
}

static void startChase(uint8_t direction) {
  const int side = (direction == DIR_A) ? CHASE_SIDE_A : CHASE_SIDE_B;

  if (chases[side].active) {
    // Triggered side is already animating; never interrupt. The bus
    // event was already sent in onTiltChange so audio still fires.
    return;
  }

  if (direction == DIR_A) {
    chases[side].frame = 0;
    chases[side].step  = 1;
  } else {
    chases[side].frame = CHASE_NUM_FRAMES - 1;
    chases[side].step  = -1;
  }
  chases[side].active     = true;
  chases[side].frameTimer = 0;
  drawSideFrame(side, chases[side].frame);
  // Push frame 0 immediately so the user sees instant feedback; the
  // tickChase pump won't advance this side until FRAME_INTERVAL_MS.
  showStrips();
}

static bool anyChaseActive() {
  for (size_t i = 0; i < CHASE_NUM_SIDES; i++) {
    if (chases[i].active) return true;
  }
  return false;
}

static void tickChase() {
  bool anyAdvanced = false;

  for (size_t i = 0; i < CHASE_NUM_SIDES; i++) {
    ChaseState &c = chases[i];
    if (!c.active) continue;
    if (c.frameTimer < FRAME_INTERVAL_MS) continue;
    c.frameTimer -= FRAME_INTERVAL_MS;

    const int next = c.frame + c.step;
    if (next < 0 || next >= (int)CHASE_NUM_FRAMES) {
      c.active = false;
      clearSidePixels((int)i);
      anyAdvanced = true;
      continue;
    }
    c.frame = next;
    drawSideFrame((int)i, c.frame);
    anyAdvanced = true;
  }

  if (anyAdvanced) showStrips();
}

static inline uint8_t scaleBrightness(uint8_t v) {
  return (uint8_t)(((uint16_t)v * (uint16_t)LED_BRIGHTNESS) / 255);
}

// Render one frame of the chase onto a single side's pair of strips.
// frameIndex is already in chase[] order: side A walks 0..N-1 (forward)
// and side B walks N-1..0 (reverse), driven by ChaseState.step in
// startChase(). Other side's pixels are NOT touched, so a concurrent
// chase on the other side keeps animating undisturbed. Pixel writes
// happen here; the actual NeoPixel show() is batched by tickChase().
static void drawSideFrame(int side, int frameIndex) {
#if SERIAL_DIAG_ENABLE
  diagChaseDraws++;
#endif
  LedStrip* const* pair = sideStrips(side);
  for (int i = 0; i < (int)STRIP_NUM_LEDS; i++) {
    const int src = i % (int)CHASE_NUM_LEDS;
    uint8_t r = scaleBrightness(pgm_read_byte(&chase[frameIndex][src * 3 + 0]));
    uint8_t g = scaleBrightness(pgm_read_byte(&chase[frameIndex][src * 3 + 1]));
    uint8_t b = scaleBrightness(pgm_read_byte(&chase[frameIndex][src * 3 + 2]));
    for (size_t s = 0; s < STRIPS_PER_SIDE; s++) {
      pair[s]->setPixel(i, r, g, b);
    }
  }
}

static void clearSidePixels(int side) {
  LedStrip* const* pair = sideStrips(side);
  for (int i = 0; i < (int)STRIP_NUM_LEDS; i++) {
    for (size_t s = 0; s < STRIPS_PER_SIDE; s++) {
      pair[s]->setPixel(i, 0, 0, 0);
    }
  }
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
  // Fade-in scale: linear ramp from 0 to 255 over IDLE_FADE_IN_MS,
  // saturating at 255 thereafter. IDLE_FADE_IN_MS == 0 disables the
  // fade and starts at full brightness immediately.
  uint8_t fadeScale = 255;
#if IDLE_FADE_IN_MS > 0
  if ((uint32_t)idleFadeTimer < (uint32_t)IDLE_FADE_IN_MS) {
    fadeScale = (uint8_t)(((uint32_t)idleFadeTimer * 255u) / (uint32_t)IDLE_FADE_IN_MS);
  }
#endif
  for (size_t s = 0; s < NUM_STRIPS; s++) {
    for (int i = 0; i < (int)STRIP_NUM_LEDS; i++) {
      uint8_t v = sampleIdleNoise((uint8_t)s, (uint16_t)i, idleNoiseZ);
      v = scaleBrightness(v);
      v = (uint8_t)(((uint16_t)v * (uint16_t)fadeScale) / 255);
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
