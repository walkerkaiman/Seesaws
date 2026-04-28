# Audio player (Raspberry Pi)

Polyphonic audio player that listens on RS485 for events from the seesaws and plays the sound mapped to each `(seesaw_id, direction)`. New events never cut off in-flight sounds. Adding a seesaw is one entry in `config.yaml` - no code changes.

See the [root README](../README.md) for system architecture and wiring.

## Layout

- [seesaw_audio.py](seesaw_audio.py) - main script
- [config.yaml](config.yaml) - serial port, audio settings, per-seesaw sound map
- [requirements.txt](requirements.txt) - Python deps
- [seesaw-audio.service](seesaw-audio.service) - optional systemd unit
- [sounds/](sounds/) - drop your WAV files here

## Hardware

A USB-to-RS485 adapter plugged into any of the Pi's USB ports. Avoid the Pi's GPIO UART for this - level mismatch (Pi GPIO is 3.3 V, classic MAX485 boards are 5 V) and Bluetooth UART contention make it more trouble than it's worth.

After plugging in, find the device:

```bash
dmesg | tail               # watch the kernel name your adapter
ls -l /dev/serial/by-id/   # stable per-device path
```

You'll see something like `/dev/ttyUSB0`. Use that in `config.yaml`. For a stable name across reboots when other USB serial devices are present, add a udev rule:

```bash
# /etc/udev/rules.d/99-seesaws.rules
SUBSYSTEM=="tty", ATTRS{idVendor}=="<vid>", ATTRS{idProduct}=="<pid>", SYMLINK+="ttyRS485"
```

Get `<vid>`/`<pid>` from `lsusb`. Then `serial.port: /dev/ttyRS485`.

## Python setup

The Pi must have system-level audio output already working (test with `aplay /usr/share/sounds/alsa/Front_Center.wav`). Then create a venv and install deps:

```bash
cd /home/pi/Seesaws/Audio
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Dependencies:

- `pyserial` - serial port I/O
- `pygame` - the actual audio mixer (SDL_mixer under the hood, polyphonic)
- `PyYAML` - config parsing

Pygame on Raspberry Pi OS may need a few SDL system packages already installed by default; if `pip install pygame` complains, run `sudo apt install python3-pygame` once or `sudo apt install libsdl2-mixer-2.0-0`.

## Configuration (`config.yaml`)

```yaml
serial:
  port: /dev/ttyUSB0     # or /dev/ttyRS485 with the udev rule above
  baud: 115200           # must match RS485_BAUD in firmware

audio:
  frequency: 44100
  buffer: 512            # smaller = lower latency, larger = fewer underruns
  channels: 32           # max simultaneous overlapping sounds
  # device: "USB Audio"  # optional: substring match for a specific output

sounds:
  1:
    A: sounds/seesaw1_A.wav
    B: sounds/seesaw1_B.wav
  2:
    A: sounds/seesaw2_A.wav
    B: sounds/seesaw2_B.wav
```

Rules:

- The numeric key under `sounds:` is the firmware's `SEESAW_ID`.
- Direction keys must be `A` or `B` and correspond to `DIR_A` / `DIR_B` in the firmware.
- Paths are resolved relative to `config.yaml` unless absolute.
- Adding a new seesaw means adding one new entry. The script does not need to be restarted unless you change the running configuration; restart on changes.

### Sound asset guidance

- **Format**: 44.1 kHz 16-bit WAV is fastest to load and lowest latency. MP3/OGG also work.
- **Length**: any length. The mixer is polyphonic, so a long sound and a short one will overlap correctly.
- **Avoid clicks**: ensure the WAV starts and ends at zero crossings. Many DAWs have a "fade-in/out 5 ms" macro for this.
- **Normalize** all sounds to roughly the same loudness to avoid surprises during installation tuning.

## Running manually (recommended for first start)

```bash
cd /home/pi/Seesaws/Audio
source .venv/bin/activate
python seesaw_audio.py
```

You should see:

```
Audio ready: 44100 Hz, buffer=512, 32 voices
Loaded seesaw 1 A -> seesaw1_A.wav
Loaded seesaw 1 B -> seesaw1_B.wav
...
Serial open: /dev/ttyUSB0 @ 115200 baud
Listening for events. Press Ctrl-C to stop.
```

Then tilt a seesaw - you should see `Playing seesaw 1 A` (or whichever direction) and hear the sound. `Ctrl-C` shuts down cleanly.

Add `-v` for debug logging including dedup events and per-frame parsing detail:

```bash
python seesaw_audio.py -v
```

## Running as a systemd service (autostart on boot)

The shipped unit assumes the repo lives at `/home/pi/Seesaws/` with the venv at `Audio/.venv/`. Adjust paths if yours differ.

```bash
sudo cp /home/pi/Seesaws/Audio/seesaw-audio.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable seesaw-audio.service
sudo systemctl start seesaw-audio.service

# verify
systemctl status seesaw-audio.service
journalctl -u seesaw-audio.service -f
```

`Restart=on-failure` brings it back if it crashes; `RestartSec=5` gives the system five seconds between attempts.

## Troubleshooting

- **No sound at all** -> confirm `aplay` works first, then check `audio.device` in config. Without it, pygame uses the default ALSA device. To force a specific output device, find its name with `aplay -L` and put a substring in `audio.device`.
- **`Sound file missing`** -> path in `config.yaml` is relative to the YAML file itself, not your shell's CWD. Either keep WAVs in `sounds/` or use absolute paths.
- **`No free channel`** -> raise `audio.channels` (try 64). Means more sounds were overlapping than the mixer was configured for.
- **`CRC mismatch` warnings** -> bus integrity issue. Usual suspects: missing termination at one end, missing bias resistors, swapped A/B somewhere, or `serial.baud` not matching the firmware's `RS485_BAUD`.
- **Garbled output / `No sound mapped for seesaw N direction X` for an N you didn't plan for** -> typically random noise on a poorly-terminated bus passing CRC by chance. Fix bus wiring and the spurious events stop.
- **Latency feels high** -> reduce `audio.buffer` (256 or 128). Trade-off: smaller buffers underrun more easily on a busy Pi.
