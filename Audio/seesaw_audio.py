#!/usr/bin/env python3
"""Polyphonic audio player for the Seesaws RS485 bus.

Reads 6-byte event frames from a serial port, validates them, and plays
the configured WAV file on a free pygame mixer channel. Multiple sounds
overlap freely - new tilt events never cut off in-flight sounds.

Wire protocol (must match Firmware/Seesaw/protocol.h):

    byte 0 : 0xAA          start-of-frame 1
    byte 1 : 0x55          start-of-frame 2
    byte 2 : id            seesaw id (1..255)
    byte 3 : event         event code (see below)
    byte 4 : seq           rolling counter
    byte 5 : crc8          CRC-8 (poly 0x07) over bytes 2..4

Event codes (byte 3):

    Tilt events - drive audio playback:
        EVT_TILT_A       (0)   SIDE_A bottomed out
        EVT_TILT_B       (1)   SIDE_B bottomed out

    State-change events - the seesaw's mode just changed:
        EVT_STATE_IDLE   (2)   entered IDLE (boot, or PLAY -> IDLE timeout)
        EVT_STATE_PLAY   (3)   entered PLAY (first tilt out of IDLE)

The firmware emits state-change events on every IDLE<->PLAY transition.
This player has a listener stub for them (see SeesawAudio.on_state_change)
that currently does nothing - the wire path is in place so idle-aware
audio behavior (attract music, prompts, etc.) can be added later
without another firmware change.
"""

from __future__ import annotations

import argparse
import logging
import queue
import signal
import sys
import threading
from collections import deque
from pathlib import Path
from typing import Deque, Dict, Optional, Tuple

import pygame
import serial
import yaml

LOG = logging.getLogger("seesaw_audio")

FRAME_SOF1 = 0xAA
FRAME_SOF2 = 0x55
FRAME_SIZE = 6

# Event codes carried in byte 3 of the frame. Must match
# Firmware/Seesaw/protocol.h.
EVT_TILT_A = 0
EVT_TILT_B = 1
EVT_STATE_IDLE = 2
EVT_STATE_PLAY = 3

TILT_EVENTS = (EVT_TILT_A, EVT_TILT_B)
STATE_EVENTS = (EVT_STATE_IDLE, EVT_STATE_PLAY)

# Single-letter labels used by the sounds config / play() logging.
DIR_NAMES = {EVT_TILT_A: "A", EVT_TILT_B: "B"}

# Long names for general event logging (state events included).
EVENT_NAMES = {
    EVT_TILT_A: "TILT_A",
    EVT_TILT_B: "TILT_B",
    EVT_STATE_IDLE: "STATE_IDLE",
    EVT_STATE_PLAY: "STATE_PLAY",
}


def crc8(data: bytes) -> int:
    """CRC-8 with polynomial 0x07 (matches firmware/protocol.h)."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


class FrameReader(threading.Thread):
    """Background thread: reads bytes from the serial port and emits
    validated (id, event, seq) tuples onto a queue."""

    def __init__(
        self,
        port: serial.Serial,
        out_queue: "queue.Queue[Tuple[int, int, int]]",
        stop_event: threading.Event,
    ):
        super().__init__(daemon=True, name="frame-reader")
        self.port = port
        self.queue = out_queue
        self.stop_event = stop_event
        self._buf = bytearray()

    def run(self) -> None:
        while not self.stop_event.is_set():
            try:
                chunk = self.port.read(64)
            except serial.SerialException:
                LOG.exception("Serial read failed")
                self.stop_event.set()
                return
            if chunk:
                self._buf.extend(chunk)
                self._consume()

    def _consume(self) -> None:
        buf = self._buf
        while True:
            i = 0
            while i + 1 < len(buf):
                if buf[i] == FRAME_SOF1 and buf[i + 1] == FRAME_SOF2:
                    break
                i += 1
            else:
                # No complete SOF found; keep last byte in case it's SOF1.
                if buf and buf[-1] == FRAME_SOF1:
                    del buf[:-1]
                else:
                    buf.clear()
                return

            if i:
                del buf[:i]
            if len(buf) < FRAME_SIZE:
                return

            sid = buf[2]
            event = buf[3]
            seq = buf[4]
            recv_crc = buf[5]
            calc_crc = crc8(bytes(buf[2:5]))
            if calc_crc != recv_crc:
                LOG.warning(
                    "CRC mismatch (got 0x%02X, want 0x%02X); resyncing",
                    recv_crc,
                    calc_crc,
                )
                # Drop one byte and try to find the next SOF.
                del buf[0]
                continue
            del buf[:FRAME_SIZE]
            self.queue.put((sid, event, seq))


class SeesawAudio:
    def __init__(self, config_path: Path):
        self.config_path = config_path
        self.config = self._load_config(config_path)
        self.serial_cfg = self.config.get("serial", {}) or {}
        self.audio_cfg = self.config.get("audio", {}) or {}
        self.sounds_cfg = self.config.get("sounds", {}) or {}
        self.config_dir = config_path.parent.resolve()

        self._dedupe: Deque[Tuple[int, int]] = deque(maxlen=128)
        self._stop_event = threading.Event()
        self._queue: "queue.Queue[Tuple[int, int, int]]" = queue.Queue()
        self._sounds: Dict[Tuple[int, int], pygame.mixer.Sound] = {}
        self._port: Optional[serial.Serial] = None

    @staticmethod
    def _load_config(path: Path) -> dict:
        with path.open("r", encoding="utf-8") as f:
            return yaml.safe_load(f) or {}

    def _resolve(self, p: str) -> Path:
        path = Path(p)
        if not path.is_absolute():
            path = self.config_dir / path
        return path

    def init_audio(self) -> None:
        freq = int(self.audio_cfg.get("frequency", 44100))
        buffer_size = int(self.audio_cfg.get("buffer", 512))
        channels = int(self.audio_cfg.get("channels", 32))
        device = self.audio_cfg.get("device")
        kwargs = {}
        if device:
            kwargs["devicename"] = device
        pygame.mixer.pre_init(
            frequency=freq,
            size=-16,
            channels=2,
            buffer=buffer_size,
            **kwargs,
        )
        pygame.mixer.init()
        pygame.mixer.set_num_channels(channels)
        LOG.info(
            "Audio ready: %d Hz, buffer=%d, %d voices%s",
            freq,
            buffer_size,
            channels,
            f", device={device}" if device else "",
        )

    def load_sounds(self) -> None:
        loaded = 0
        for sid_raw, sides in self.sounds_cfg.items():
            try:
                sid = int(sid_raw)
            except (TypeError, ValueError):
                LOG.error("Skipping non-integer seesaw id: %r", sid_raw)
                continue
            for side_label, file_path in (sides or {}).items():
                if file_path is None:
                    continue
                key_label = str(side_label).upper()
                if key_label not in ("A", "B"):
                    LOG.error("Unknown direction %r for seesaw %d", side_label, sid)
                    continue
                key = (sid, 0 if key_label == "A" else 1)
                full = self._resolve(file_path)
                if not full.exists():
                    LOG.error("Sound file missing for seesaw %d %s: %s", sid, key_label, full)
                    continue
                try:
                    self._sounds[key] = pygame.mixer.Sound(str(full))
                    loaded += 1
                    LOG.info("Loaded seesaw %d %s -> %s", sid, key_label, full.name)
                except pygame.error as e:
                    LOG.error("Failed to load %s: %s", full, e)
        LOG.info("Loaded %d sound files", loaded)
        if loaded == 0:
            LOG.warning("No sounds loaded; events will be ignored")

    def open_port(self) -> None:
        port_name = self.serial_cfg.get("port", "/dev/ttyUSB0")
        baud = int(self.serial_cfg.get("baud", 115200))
        self._port = serial.Serial(
            port=port_name,
            baudrate=baud,
            timeout=0.1,
        )
        LOG.info("Serial open: %s @ %d baud", port_name, baud)

    def play(self, sid: int, direction: int) -> None:
        sound = self._sounds.get((sid, direction))
        label = DIR_NAMES.get(direction, str(direction))
        if sound is None:
            LOG.warning("No sound mapped for seesaw %d direction %s", sid, label)
            return
        channel = pygame.mixer.find_channel(True)
        if channel is None:
            LOG.warning("No free channel for seesaw %d %s", sid, label)
            return
        channel.play(sound)
        LOG.info("Playing seesaw %d %s", sid, label)

    def on_state_change(self, sid: int, event: int, seq: int) -> None:
        """Listener stub for IDLE<->PLAY state-change events from a seesaw.

        The firmware emits EVT_STATE_IDLE when a seesaw enters its idle
        animation (on boot, or after IDLE_TIMEOUT_MS without a tilt) and
        EVT_STATE_PLAY when it transitions to play (the first tilt out
        of idle). Each event is sent on RS485 like a regular tilt frame
        and is delivered to this method after dedupe.

        This is intentionally a no-op placeholder today - it just logs
        the event so you can verify on the bench that state changes
        reach the Pi. Hook idle-aware audio behavior (attract music,
        prompts, ducking, ...) in here when you're ready; the firmware
        already emits the events, no firmware change needed to start
        acting on them.
        """
        name = EVENT_NAMES.get(event, f"0x{event:02X}")
        LOG.info("State change: seesaw %d -> %s (seq %d)", sid, name, seq)

    def run(self) -> int:
        self.init_audio()
        self.load_sounds()
        self.open_port()

        assert self._port is not None
        reader = FrameReader(self._port, self._queue, self._stop_event)
        reader.start()

        def _shutdown(_sig, _frame):
            LOG.info("Shutdown requested")
            self._stop_event.set()

        signal.signal(signal.SIGINT, _shutdown)
        signal.signal(signal.SIGTERM, _shutdown)

        LOG.info("Listening for events. Press Ctrl-C to stop.")
        try:
            while not self._stop_event.is_set():
                try:
                    sid, event, seq = self._queue.get(timeout=0.2)
                except queue.Empty:
                    continue
                key = (sid, seq)
                if key in self._dedupe:
                    LOG.debug("Dedup seesaw %d seq %d", sid, seq)
                    continue
                self._dedupe.append(key)
                if event in TILT_EVENTS:
                    self.play(sid, event)
                elif event in STATE_EVENTS:
                    self.on_state_change(sid, event, seq)
                else:
                    LOG.warning(
                        "Unknown event code 0x%02X from seesaw %d (seq %d)",
                        event,
                        sid,
                        seq,
                    )
        finally:
            self._stop_event.set()
            if self._port is not None:
                try:
                    self._port.close()
                except Exception:
                    pass
            pygame.mixer.quit()
        return 0


def main() -> int:
    p = argparse.ArgumentParser(description="Seesaw RS485 polyphonic audio player")
    p.add_argument(
        "-c", "--config",
        type=Path,
        default=Path(__file__).resolve().parent / "config.yaml",
        help="Path to config.yaml (default: alongside this script)",
    )
    p.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable debug logging",
    )
    args = p.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )

    if not args.config.exists():
        LOG.error("Config not found: %s", args.config)
        return 2

    return SeesawAudio(args.config).run()


if __name__ == "__main__":
    sys.exit(main())
