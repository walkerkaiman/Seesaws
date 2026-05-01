#pragma once

// =====================================================================
// Seesaw firmware configuration
// =====================================================================
//
// Edit these values per seesaw before flashing. SEESAW_ID must be unique
// across the bus and must match an entry in the Pi's config.yaml.
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
//   - IDLE: continuously loops the idle animation in idle.h on all
//     four strips with a per-strip frame offset, so the four strips
//     animate phase-shifted.
//
// Boot starts in IDLE so the seesaw shows the idle animation
// immediately on power-up. After IDLE_TIMEOUT_MS without a tilt event,
// the firmware drops back from PLAY to IDLE; a tilt while idle
// instantly returns it to PLAY.

// Time (ms) without a tilt event before reverting from PLAY to IDLE.
// Default 60000 ms = 60 seconds.
#define IDLE_TIMEOUT_MS  60000

// Idle animation frame rate (frames per second). Independent of
// CHASE_FPS so you can have a slow breathing idle and a fast play
// chase, or vice versa.
#define IDLE_FPS         15

// Per-strip frame offsets for the idle animation. Each strip displays
// the idle chase but shifted by this many frames (modulo IDLE_NUM_FRAMES
// from idle.h), creating a phase-shifted wave across the four strips.
// The defaults divide the cycle into quarters between A1, A2, B1, B2;
// override here if you want a different idle pattern (e.g. set both
// pins on a side to the same offset to keep the pair in lock-step).
#define IDLE_FRAME_OFFSET_A1  0
#define IDLE_FRAME_OFFSET_A2  ((IDLE_NUM_FRAMES * 1) / 4)
#define IDLE_FRAME_OFFSET_B1  ((IDLE_NUM_FRAMES * 2) / 4)
#define IDLE_FRAME_OFFSET_B2  ((IDLE_NUM_FRAMES * 3) / 4)

// Global LED brightness scaler, 0..255. Every R/G/B value coming out of
// the chase data is multiplied by (LED_BRIGHTNESS / 255) before being
// written to the strips, so 255 = full brightness (chase data unchanged)
// and 0 = strips dark. Lower this to cap power draw or tame an
// over-bright install without re-rendering the chase animation.
//
// Rough WS2813 power scaling at 5 V (per LED, full white frame):
//   255 -> ~60 mA    192 -> ~45 mA    128 -> ~30 mA    64 -> ~15 mA
#define LED_BRIGHTNESS 255

// ---- Pin assignments (Teensy 4.0) -----------------------------------
//
//   I2C_SDA / I2C_SCL    - MPU6050 accelerometer over the default Wire bus.
//                          Teensy 4.0 Wire = pin 18 (SDA) / pin 19 (SCL).
//                          Module already has built-in 4.7k pull-ups, so no
//                          external resistors needed.
//   PIN_LED_STRIP_A1/A2  - SIDE_A LED strip pair. Both pins receive the
//   PIN_LED_STRIP_B1/B2    same frame data as their side mate; only one
//                          pair lights at a time. A DIR_A event runs the
//                          chase forward on the SIDE_A pair (B pair stays
//                          dark); a DIR_B event runs it in reverse on the
//                          SIDE_B pair (A pair stays dark). Each pin is
//                          routed through a 74AHCT125 (5V) buffer before
//                          reaching its strip.
//                          Valid Teensy 4.0 WS2812Serial pins: 1 (Serial1,
//                          taken by RS485), 8 (Serial2), 14 (Serial3),
//                          17 (Serial4), 20 (Serial5), 24 (Serial6),
//                          29 (Serial7), 39 (Serial8).
//   PIN_RS485_DE         - Tied to MAX3485 DE+RE; toggled automatically by
//                          Serial1.transmitterEnable().
#define PIN_LED_STRIP_A1   8
#define PIN_LED_STRIP_A2  14
#define PIN_LED_STRIP_B1  17
#define PIN_LED_STRIP_B2  20
#define PIN_RS485_DE       6

// ---- RS485 ----------------------------------------------------------
#define RS485_BAUD        115200

// Each event is sent on the bus this many times with random jitter
// between sends, to mitigate rare collisions. The Pi dedupes by (id, seq).
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
