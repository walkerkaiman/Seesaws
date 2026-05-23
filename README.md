# Seesaws

Distributed control system for an interactive seesaw installation. Each seesaw has a **Teensy 4.0** that detects rocking with an **MPU6050** accelerometer, plays a directional LED chase on whichever pair of WS2813 strips just bottomed out, and broadcasts the event over an **RS485** bus to a central **Teensy 4.0 with Audio Shield (Rev D)** that plays a sound from SD. Sounds are polyphonic and overlap freely instead of cutting each other off.

The system scales to N seesaws with no architectural changes: every seesaw runs identical firmware, only `SEESAW_ID` differs per flash, and adding audio for a new seesaw is two WAV files on the SD card named `{id}_A.wav` and `{id}_B.wav`.

## Repo layout

- [Firmware/](Firmware/) - per-seesaw Teensy 4.0 firmware. See [Firmware/README.md](Firmware/README.md).
- [Audio/](Audio/) - central audio node (Teensy 4.0 + Audio Shield Rev D). See [Audio/README.md](Audio/README.md).

## Behavior

The firmware runs in one of two modes per seesaw:

- **IDLE** (boot default; re-entered after `IDLE_TIMEOUT_MS` of no tilt activity): a procedural grayscale 3D noise field (FastLED `inoise8`) drifts continuously across all four strips. No CSV; the field never repeats.
- **PLAY** (entered on the first tilt): one LED chase per side, each pipeline independent. A side's chase is *never* interrupted - if the same side fires again while its chase is still running, the bus event still goes out (so audio plays) but the LEDs keep playing the current frame sequence to completion. Both sides can chase concurrently when the rider rocks faster than one chase plays out.

Tilt detection uses the accelerometer along the seesaw's length:

1. The filtered tilt signal must enter an A-down or B-down zone (`TILT_FIRE_THRESHOLD_G` to enter, `TILT_RELEASE_THRESHOLD_G` to leave - Schmitt-trigger hysteresis on the boundaries).
2. While in that zone, the firmware tracks the running peak. The trigger fires the first time the signal backs off the peak by `TILT_REVERSAL_DELTA_G` - i.e. the seesaw has actually started reversing direction.
3. An armed-side flag enforces alternation: once one side fires, the same side cannot fire again until the other side has fired.

On every fire:

1. The Teensy sends a 6-byte event frame over RS485 announcing `(SEESAW_ID, direction)`.
2. If the seesaw was in IDLE, it switches to PLAY (and the running idle noise is wiped). The triggered side's LED chase starts on its pair of strips - the SIDE_A pair runs the chase forward on `DIR_A`, the SIDE_B pair runs it in reverse on `DIR_B`.
3. The central audio Teensy receives the frame, validates the CRC, dedupes against duplicate retransmits, and plays `sounds/{seesaw_id}_A.wav` or `sounds/{seesaw_id}_B.wav` from the Audio Shield SD card on a free mixer voice.

After `IDLE_TIMEOUT_MS` of no further tilt events, and only once **both** chase pipelines are idle, the seesaw drops back to IDLE.

The firmware also sends an `EVT_STATE_*` frame on every IDLE<->PLAY transition (boot lands in IDLE, the first tilt out of IDLE flips to PLAY, idle timeout drops back to IDLE). On an IDLE -> PLAY transition the state-change event goes out before the tilt event that caused it. The audio Teensy has a no-op listener stub for these (USB Serial log only today); see [Audio/README.md](Audio/README.md#state-change-events-idleplay) for hooking idle-aware audio in later.

The audio node is a passive listener; seesaws never wait for an ack. Each event is sent twice on the bus with random jitter to mitigate the rare case of two seesaws tilting simultaneously.

## Hardware (brief)

Per seesaw:

- 1x Teensy 4.0
- 1x MPU6050 breakout (e.g. GY-521), mounted with one axis along the seesaw length
- 1x **3.3 V** RS485 transceiver (MAX3485 or SN65HVD3082) - **not** the 5 V MAX485
- 1x 74AHCT125 (or similar) 5 V level-shift buffer for the four LED data lines
- 4x WS2813 strips, default 45 LEDs each, two per side. Each strip wants a 1000 uF cap at the start and ~470 ohm in series with its data line.
- 1x 24 V to 5 V buck converter sized for the seesaw's worst-case 5 V draw (Pololu D24V90F5 5 A is enough for 4 x 45 LEDs at default brightness)
- Inline fuse on the 24 V tap and a small IP65 junction box to weatherproof the buck

Central rack:

- 1x Teensy 4.0 + PJRC Audio Shield Rev D with microSD
- 1x **3.3 V** RS485 transceiver on Serial1
- 1x 24 V central PSU sized to the whole installation
- Audio amp + speakers (or powered speakers off line-out)
- 120 ohm termination across A-B at this end of the bus, plus ~680 ohm bias resistors A-to-3V3 and B-to-GND. The far end of the bus (last seesaw) gets the other 120 ohm termination.

Bus cable carries `+24V`, `GND` (heavy-ish gauge, doubles as RS485 ground reference), `A`, and `B`.

## Wiring (brief)

Per seesaw, all on the same Teensy 4.0:

| Function | Pin |
|---|---|
| LED data A1 / A2 (SIDE_A pair) | 6 / 7 |
| LED data B1 / B2 (SIDE_B pair) | 8 / 9 |
| RS485 RX / TX / DE+RE | 0 / 1 / 2 |
| MPU6050 SDA / SCL | 18 / 19 |
| 5 V power in | VIN (cut the VIN/VUSB pad if you also program over USB) |

Detailed signal flow lives in [Firmware/README.md](Firmware/README.md#pin-map). LED data lines go through the 74AHCT125 (5 V powered) before the 470 ohm series resistors. The MAX3485 runs from the Teensy's 3V3 rail. Strips are powered directly from the buck's 5 V output, never through the Teensy.

## Installation workflow

1. **Wire one seesaw** per the brief table above. Power it up; the procedural noise idle should appear on all four strips immediately.
2. **Set the seesaw's ID** by editing `#define SEESAW_ID 1` in [Firmware/Seesaw/config.h](Firmware/Seesaw/config.h). Use `1`, `2`, ... per board.
3. **Tune tilt** with the install-time procedure in [Firmware/README.md](Firmware/README.md#tilt-detection-tuning). Briefly: at rest, read the periodic `accel(g) y=` line and use that for `TILT_ZERO_OFFSET_G`; push side A all the way down and pick `TILT_FIRE_THRESHOLD_G` at ~60-70% of the bottomed-out reading.
4. **Build animation data** for the play chase if you don't want the placeholder:
   ```bash
   python Firmware/tools/csv_to_header.py path/to/chase.csv
   ```
   The default firmware does NOT use a CSV idle - idle is procedural noise tuned via constants in [Firmware/Seesaw/config.h](Firmware/Seesaw/config.h).
5. **Flash** with the Arduino IDE + Teensyduino. Pin map and library list are in [Firmware/README.md](Firmware/README.md).
6. **Verify both modes:**
   - **Idle**: at power-up, all four strips drift through grayscale noise.
   - **Play (Side A)**: rock the seesaw so Side A bottoms out and starts coming back up. The SIDE_A pair runs the chase forward; the SIDE_B pair stays dark. Watch USB Serial for `TILT dir=A`.
   - **Play (Side B)**: rock the other way. The SIDE_B pair runs the same chase in reverse.
   - **Idle return**: leave the seesaw alone for `IDLE_TIMEOUT_MS` (default 60 s). Idle noise resumes.
7. **Repeat** steps 2-6 for each seesaw, incrementing the ID.
8. **Set up the audio Teensy**: flash [Audio/SeesawAudio](Audio/SeesawAudio), copy `Audio/sounds/` onto the SD card as `sounds/{id}_A.wav` and `sounds/{id}_B.wav`. See [Audio/README.md](Audio/README.md).
9. **Wire the bus** with termination at both physical ends and biasing at the rack end. Connect the audio Teensy's transceiver to A/B/GND.
10. **Power the audio Teensy** - it starts listening on boot. Use USB Serial (115200) for bench verification.

## Sub-READMEs

- [Firmware/README.md](Firmware/README.md) - Teensyduino setup, library list, pin map, peak-on-reversal tilt detection, idle noise tuning, animation workflow, troubleshooting.
- [Audio/README.md](Audio/README.md) - Teensy 4.0 + Audio Shield Rev D setup, SD card naming, Teensyduino flash, troubleshooting.

## Defaults

| Setting | Default | Where to change |
|---|---|---|
| Strip length | 45 LEDs | `STRIP_NUM_LEDS` in [Firmware/Seesaw/config.h](Firmware/Seesaw/config.h) |
| Play frame rate | 30 FPS | `CHASE_FPS` |
| Idle frame rate | 30 FPS | `IDLE_FPS` |
| Idle timeout | 60 s | `IDLE_TIMEOUT_MS` |
| Idle noise tuning | see [Firmware/README.md](Firmware/README.md#idle-animation-procedural-noise) | `IDLE_NOISE_*` constants |
| Tilt accel axis | Y (along seesaw length) | `TILT_AXIS_ACCEL` |
| Tilt zone enter / exit | 0.30 g / 0.18 g | `TILT_FIRE_THRESHOLD_G` / `TILT_RELEASE_THRESHOLD_G` |
| Reversal delta | 0.05 g | `TILT_REVERSAL_DELTA_G` |
| LPF alpha | 0.20 | `TILT_LPF_ALPHA_X1000` (stored as int * 1000) |
| Tilt zero offset | 0.00 g | `TILT_ZERO_OFFSET_G` |
| Tilt sample rate | 100 Hz | `TILT_SAMPLE_INTERVAL_MS` |
| RS485 baud | 115200 | `RS485_BAUD` in seesaw firmware AND [Audio/SeesawAudio/config.h](Audio/SeesawAudio/config.h) |
| Resend count | 2 | `RS485_RESEND_COUNT` |
| Polyphony | 8 voices | `MAX_VOICES` in [Audio/SeesawAudio/config.h](Audio/SeesawAudio/config.h) |
