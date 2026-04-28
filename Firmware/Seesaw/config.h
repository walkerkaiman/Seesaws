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

// Pin assignments (Teensy 4.0).
//
//   PIN_TILT          - Ball/mercury tilt switch to GND (uses INPUT_PULLUP).
//   PIN_LED_STRIP_1/2 - WS2812Serial-supported TX pins. Both strips are
//                       driven with the same data simultaneously. Routed
//                       through a 74AHCT125 (5V) buffer before reaching
//                       the strips.
//                       Valid Teensy 4.0 pins: 1 (Serial1, taken by RS485),
//                       8 (Serial2), 14 (Serial3), 17 (Serial4), 20 (Serial5),
//                       24 (Serial6), 29 (Serial7), 39 (Serial8).
//   PIN_RS485_DE      - Tied to MAX3485 DE+RE; toggled automatically by
//                       Serial1.transmitterEnable().
#define PIN_TILT          2
#define PIN_LED_STRIP_1   8
#define PIN_LED_STRIP_2   14
#define PIN_RS485_DE      6

// RS485 line settings. The Pi audio player must match.
#define RS485_BAUD        115200

// Tilt debounce window. The switch must read the same value for at least
// this many ms before the change is accepted.
#define TILT_DEBOUNCE_MS  50

// Each event is sent on the bus this many times with random jitter
// between sends, to mitigate rare collisions. The Pi dedupes by (id, seq).
#define RS485_RESEND_COUNT          2
#define RS485_RESEND_JITTER_MIN_MS  5
#define RS485_RESEND_JITTER_MAX_MS  25
