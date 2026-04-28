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

// Animation frame rate (frames per second).
#define CHASE_FPS 30

// ---- Pin assignments (Teensy 4.0) -----------------------------------
//
//   I2C_SDA / I2C_SCL  - MPU6050 accelerometer over the default Wire bus.
//                        Teensy 4.0 Wire = pin 18 (SDA) / pin 19 (SCL).
//                        Module already has built-in 4.7k pull-ups, so no
//                        external resistors needed.
//   PIN_LED_STRIP_1/2  - WS2812Serial-supported TX pins. Both strips are
//                        driven with the same data simultaneously. Routed
//                        through a 74AHCT125 (5V) buffer before reaching
//                        the strips.
//                        Valid Teensy 4.0 pins: 1 (Serial1, taken by RS485),
//                        8 (Serial2), 14 (Serial3), 17 (Serial4), 20 (Serial5),
//                        24 (Serial6), 29 (Serial7), 39 (Serial8).
//   PIN_RS485_DE       - Tied to MAX3485 DE+RE; toggled automatically by
//                        Serial1.transmitterEnable().
#define PIN_LED_STRIP_1   8
#define PIN_LED_STRIP_2   14
#define PIN_RS485_DE      6

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
