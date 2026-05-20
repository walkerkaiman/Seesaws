#pragma once

// =====================================================================
// Seesaw firmware configuration
// =====================================================================
//
// Edit these values per seesaw before flashing. SEESAW_ID must be unique
// across the bus. The central audio Teensy plays sounds/{SEESAW_ID}_A.wav
// and sounds/{SEESAW_ID}_B.wav from its SD card when that id tilts.
//
// =====================================================================

// Unique 1..255 identifier for this seesaw.
#define SEESAW_ID 1

// Number of LEDs on each WS2813 strip. This is a *hardware* property -
// all four physical strips on the seesaw must be this length. The chase
// data in chase.h has its own width (CHASE_NUM_LEDS, auto-derived by
// csv_to_header.py from the source CSV); it must be <= STRIP_NUM_LEDS.
// Any LEDs past CHASE_NUM_LEDS are written black on every clear and
// otherwise left untouched, so they stay dark.
#define STRIP_NUM_LEDS 45

// Animation frame rate for the play-mode chase (frames per second).
#define CHASE_FPS 30

// ---- Idle / Play state machine -------------------------------------
//
// The seesaw runs in one of two modes at any time:
//   - PLAY: triggered by a tilt event; runs the chase in chase.h on
//     the pair of strips on the side that just bottomed out.
//   - IDLE: continuously samples a procedural grayscale 3D noise field
//     (FastLED inoise8) on all four strips. The noise field's z axis
//     advances slowly each frame, so the animation can run forever
//     without a visible loop or repeat.
//
// Boot starts in IDLE so the seesaw shows the idle animation
// immediately on power-up. After IDLE_TIMEOUT_MS without a tilt event,
// the firmware drops back from PLAY to IDLE; a tilt while idle
// instantly returns it to PLAY.

// Time (ms) without a tilt event before reverting from PLAY to IDLE.
// Default 6000 ms = 6 seconds.
#define IDLE_TIMEOUT_MS  6000

// Idle animation frame rate (frames per second). 30 is the design target
// for the procedural noise idle (smooth drift). Independent of CHASE_FPS.
#define IDLE_FPS         30

// ---- Idle noise tuning (see idle_noise.h) --------------------------
//
// IDLE_NOISE_Z_STEP    z-axis (time) advance per idle tick. Smaller =
//                      slower drift. At IDLE_FPS=30 a step of 4 means
//                      noise features pass roughly every ~0.5 s.
// IDLE_NOISE_X_SCALE   noise-space gap between adjacent LEDs. Too small
//                      (<20) and neighbors look identical (smooth blob);
//                      too large (>80) and the strip looks like static
//                      sparkle. inoise8's feature length is ~64-128
//                      units, so 30..50 gives clearly distinct pixels
//                      that still group into visible blobs of ~3-5 LEDs.
// IDLE_NOISE_Y_STRIDE  noise-space gap between adjacent strips. 0 makes
//                      all four strips identical. >=128 makes them look
//                      effectively independent.
// IDLE_NOISE_Y_BASE    constant added to y for strip 0. Use to slide
//                      the whole pattern across without changing speed.
// IDLE_NOISE_PER_ID_OFFSET
//                      per-seesaw spatial bias added to both x and y so
//                      neighboring seesaws don't render identical
//                      patterns at the same z. 0 disables.
#define IDLE_NOISE_Z_STEP          8
#define IDLE_NOISE_X_SCALE         20
#define IDLE_NOISE_Y_STRIDE        192
#define IDLE_NOISE_Y_BASE          0
#define IDLE_NOISE_PER_ID_OFFSET   73

// inoise8() output clusters in roughly IDLE_NOISE_FLOOR..IDLE_NOISE_CEIL
// instead of spanning 0..255. Without this stretch, every pixel lives
// in the bright midrange and the strip reads as solid warm white. The
// renderer remaps [FLOOR..CEIL] -> [0..255] (saturating outside) so the
// noise actually reaches both fully dark and fully bright.
//
// To disable the stretch, set FLOOR=0 and CEIL=255 (identity mapping).
// CEIL must be strictly greater than FLOOR.
#define IDLE_NOISE_FLOOR           80
#define IDLE_NOISE_CEIL            150

// Global LED brightness scaler, 0..255. Every R/G/B value coming out of
// the chase data or noise sampler is multiplied by (LED_BRIGHTNESS/255)
// before being written to the strips, so 255 = full brightness (data
// unchanged) and 0 = strips dark. Lower this to cap power draw or tame
// an over-bright install without re-rendering animations.
//
// Rough WS2813 power scaling at 5 V (per LED, full white frame):
//   255 -> ~60 mA    192 -> ~45 mA    128 -> ~30 mA    64 -> ~15 mA
#define LED_BRIGHTNESS 128

// ---- USB Serial diagnostics -----------------------------------------
//
// Open the Teensy USB Serial Monitor at SERIAL_BAUD while bench-testing.
// Periodic status lines print every SERIAL_DIAG_INTERVAL_MS; tilt/state
// transitions and LED draws are logged immediately when enabled.
#define SERIAL_DIAG_ENABLE          1
#define SERIAL_BAUD                 115200
#define SERIAL_DIAG_INTERVAL_MS     2000

// At boot, flash every LED on all four strips (verifies wiring/power).
// Set 0 once strips are confirmed working.
#define BENCH_LED_SELFTEST          0

// ---- Pin assignments (Teensy 4.0) -----------------------------------
//
//   I2C_SDA / I2C_SCL    - MPU6050 accelerometer over the default Wire bus.
//                          Teensy 4.0 Wire = pin 18 (SDA) / pin 19 (SCL).
//                          Module already has built-in 4.7k pull-ups, so no
//                          external resistors needed.
//   PIN_LED_STRIP_A1/A2  - SIDE_A LED strip pair (GPIO 6/7 on this install).
//   PIN_LED_STRIP_B1/B2  - SIDE_B pair (GPIO 8/9). Driven with Adafruit_NeoPixel
//                          (any GPIO). show() briefly masks interrupts; RS485 RX
//                          relies on the Serial1 hardware FIFO during LED updates.
//   Serial1 (RS485): RX=0, TX=1, DE+RE=2 (PIN_RS485_DE). DE/RE must not
//   overlap LED data pins 6..9.
#define PIN_LED_STRIP_A1  6
#define PIN_LED_STRIP_A2  7
#define PIN_LED_STRIP_B1  8
#define PIN_LED_STRIP_B2  9
#define PIN_RS485_DE      2    // MAX3485 DE+RE tied together; Serial1.transmitterEnable()

// ---- RS485 ----------------------------------------------------------
#define RS485_BAUD        115200

// Each event is sent on the bus this many times with random jitter
// between sends, to mitigate rare collisions. The audio node dedupes by (id, seq).
#define RS485_RESEND_COUNT          2
#define RS485_RESEND_JITTER_MIN_MS  5
#define RS485_RESEND_JITTER_MAX_MS  25

// ---- Tilt detection (MPU6050 gyro reversal) -------------------------
//
// Tilt events fire the moment the seesaw *reverses direction* - when one
// side reaches its lowest point and starts coming back up. This is the
// "thump" / impact moment, and it works at any amplitude: a small child
// who only rocks the seesaw a few degrees and an adult who swings through
// 30 degrees both reliably trigger the same way.
//
// The MPU6050's gyroscope reports angular velocity (deg/s) around three
// axes. Pick the axis aligned with the seesaw's *rotation* axis - this is
// the axis the seesaw rotates around, typically perpendicular to the
// seesaw's length. With a typical breakout sitting flat on the seesaw
// deck and X aligned to the length, the seesaw rotates around Y, so
// TILT_GYRO_AXIS = TILT_GYRO_AXIS_Y.
//
// Convention: negative velocity means moving toward SIDE_A; positive
// means moving toward SIDE_B. Use TILT_INVERT to flip this if your
// mounting gives the opposite sign.
//
// State machine:
//   - Below |TILT_MIN_VELOCITY_DPS| we hold the previous direction
//     (so noise around zero cannot produce fake reversals).
//   - Crossing from "moving toward A" to "moving toward B" fires DIR_A
//     (side A just peaked). Symmetrically for DIR_B.
//   - After firing we suppress further events for TILT_EVENT_COOLDOWN_MS
//     so a fast bounce can't re-interrupt the chase.

#define MPU_I2C_ADDR                  0x68    // 0x68 default; 0x69 if AD0 = HIGH

#define TILT_GYRO_AXIS_X 0
#define TILT_GYRO_AXIS_Y 1
#define TILT_GYRO_AXIS_Z 2
#define TILT_GYRO_AXIS                TILT_GYRO_AXIS_Y
#define TILT_INVERT                   false   // true if positive velocity should map to SIDE_A

// Minimum sustained angular velocity (deg/s) to count as real motion.
// MPU6050 noise is well under 1 dps, so 10-20 dps catches even gentle
// seesaw motion while ignoring vibration. Lower = more sensitive.
#define TILT_MIN_VELOCITY_DPS         15.0f

// After firing an event, suppress further events for this many ms.
// Prevents a fast-bounce double-trigger from re-interrupting the chase.
// 150 ms allows up to ~6 events/s - faster than humans can rock.
#define TILT_EVENT_COOLDOWN_MS        150

// Gyro sampling. 100 Hz catches the reversal moment within ~10 ms.
#define TILT_SAMPLE_INTERVAL_MS       10
