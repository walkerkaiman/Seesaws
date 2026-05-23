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

// Fade-in duration when entering IDLE (ms). The grayscale noise field
// starts at 0% brightness and ramps linearly to 100% over this many
// milliseconds. Applies to both the boot-into-IDLE transition and
// every PLAY -> IDLE transition (so the chase doesn't snap back to a
// bright shimmer the instant the timeout expires). Set to 0 to disable.
#define IDLE_FADE_IN_MS  3000

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

// ---- Tilt detection (MPU6050 accel peak-on-reversal) ----------------
//
// Triggers fire when the seesaw bottoms out on one side and starts
// reversing -- the "thump" moment the rider feels. The accelerometer
// signal along the seesaw length is low-pass filtered; once it crosses
// TILT_FIRE_THRESHOLD_G on one side we enter that side's zone and start
// tracking a running peak. The fire happens the first time the filtered
// signal drops back from that peak by at least TILT_REVERSAL_DELTA_G --
// i.e. the seesaw has actually started swinging the other way. The
// signal must come back through TILT_RELEASE_THRESHOLD_G (hysteresis)
// before that side can re-arm. An armed-side flag enforces alternation
// -- the same side never fires twice in a row, regardless of bounce or
// hand-jitter.
//
// Sensor-to-seesaw axis mapping (this build):
//
//   The MPU-6050 die has +X right, +Y up, +Z out of the package when
//   pin 1 (the chip's dot marker) is at the top-left of the die. The
//   GY-521 breakout puts that dot at the bottom-left of the board with
//   the chip rotated 90 degrees clockwise relative to the silkscreen,
//   so on this install:
//
//     - chip +Y points along the right edge of the breakout, which
//       this install aligns with the seesaw's *length* (perpendicular
//       to the pivot). Tilting projects gravity onto accel Y as
//       g * sin(angle); this is the trigger signal.
//     - chip +X points along the bottom edge -- the seesaw's *pivot
//       rotation* axis. Gyro X carries rocking velocity; we read it
//       only for diagnostics.
//     - chip +Z is the deck normal.
//
//   If a future build mounts the breakout differently, change
//   TILT_AXIS_ACCEL / TILT_AXIS_GYRO. Set TILT_INVERT_ACCEL = true if
//   pushing SIDE_A down makes accel-on-the-length go negative.

#define MPU_I2C_ADDR                  0x68    // 0x68 default; 0x69 if AD0 = HIGH

#define TILT_AXIS_X 0
#define TILT_AXIS_Y 1
#define TILT_AXIS_Z 2

// Which accelerometer axis lies along the seesaw length. Sign + means
// SIDE_A is "down" (gravity pulls chip in this direction). Flip
// TILT_INVERT_ACCEL if your mounting gives the opposite polarity.
#define TILT_AXIS_ACCEL               TILT_AXIS_Y
#define TILT_INVERT_ACCEL             false

// Gyro axis along the pivot. Used only for the periodic diagnostic
// printout; not part of the trigger logic.
#define TILT_AXIS_GYRO                TILT_AXIS_X

// Schmitt-trigger thresholds for entering / leaving an A-down or
// B-down zone, expressed in g (accel reading after offset + invert).
//   FIRE: must cross to enter the zone and start watching for reversal.
//   RELEASE: must come back inside this band before the same side can
//            re-arm. RELEASE strictly less than FIRE.
// 0.30 g is roughly 17 deg of tilt; 0.18 g is roughly 10 deg. Tune to
// your mechanical range; pick FIRE at ~60-70% of bottomed-out reading.
#define TILT_FIRE_THRESHOLD_G         0.30f
#define TILT_RELEASE_THRESHOLD_G      0.18f

// Reversal detection: fire only after the filtered accel has come
// back from its peak inside the active zone by at least this many g.
// 0.05 g is roughly 3 deg of swing back, which is well above noise
// at the LPF cutoff but still much smaller than a real seesaw stroke.
// Bumping this delays the fire toward the actual peak (more "thump"
// feel, less responsive); shrinking it fires earlier but is more
// sensitive to chatter at the peak.
#define TILT_REVERSAL_DELTA_G         0.05f

// Single-pole IIR low-pass on the accel-along-length signal. Stored
// as alpha * 1000 so the macro is unambiguous integer.
//   filtered += alpha * (sample - filtered)
// At 100 Hz sample rate, alpha = 0.20 gives roughly a 3 Hz cutoff,
// which kills vibration without lagging perceived seesaw motion.
#define TILT_LPF_ALPHA_X1000          200

// Subtract from raw accel-along-length before thresholding. Use this
// only if the seesaw doesn't sit at 0 g on this axis when undisturbed
// (mounting offset, deck not perfectly level). Read the periodic diag
// "filteredG=" line at rest to pick a value.
#define TILT_ZERO_OFFSET_G            0.00f

// Sample period. 100 Hz is plenty for human-rocking timescales and
// gives the LPF a useful cutoff.
#define TILT_SAMPLE_INTERVAL_MS       10
