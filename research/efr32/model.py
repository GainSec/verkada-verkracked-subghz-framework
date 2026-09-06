#!/usr/bin/env python3
"""Deterministic interoperability model of BH-series parser and peer state.

The model is offline. It opens no radio, serial, USB, or network interface. It
models independently reconstructed framing and normal state transitions. It is
not a cycle-accurate EFR32 emulator and intentionally excludes security-defect
and firmware-update behavior.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import struct


PENDING_LIFETIME_MS = 15_000
JOINED_LIFETIME_MS = 4 * 60 * 60 * 1000
PEER_CAPACITY = 32
REPLAY_AHEAD = 100
class FrameRejected(ValueError):
    """A frame failed a recovered parser boundary before state mutation."""


def crc16_ccitt(data: bytes) -> int:
    crc = 0
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = (((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)) & 0xFFFF
    return crc


def crc16_radio(data: bytes) -> int:
    crc = 0
    for value in data:
        reflected = int(f"{value:08b}"[::-1], 2)
        crc ^= reflected << 8
        for _ in range(8):
            crc = (((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)) & 0xFFFF
    return crc


def build_v4_join_body(serial: bytes, *, regulatory: int = 1) -> bytes:
    if not 0 < len(serial) <= 0xFF:
        raise ValueError("v4 serial length must be 1..255")
    if not 0 <= regulatory <= 0xFF:
        raise ValueError("regulatory code must be one byte")
    return (
        b"\x04\x00" + bytes(4) + bytes(8) + bytes([len(serial)]) + serial
        + bytes(8) + bytes([0, regulatory])
    )


def build_plaintext_frame(
    vcmp_type: int,
    body: bytes,
    source_eui: bytes,
    sequence: int = 0,
    *,
    pan: int = 0x01FF,
    destination: int = 1,
) -> bytes:
    if len(source_eui) != 8:
        raise ValueError("source EUI-64 must be eight bytes")
    if not 0 <= vcmp_type <= 0xFF or not 0 <= sequence <= 0xFF:
        raise ValueError("type and sequence must be one byte")
    type_flags = bytes([vcmp_type, 0])
    inner_crc = crc16_ccitt(type_flags + b"\x00\x00" + body)
    psdu = (
        b"\x41\xc8" + bytes([sequence]) + struct.pack("<H", pan)
        + struct.pack("<H", destination) + source_eui + type_flags
        + struct.pack(">H", inner_crc) + body
    )
    phr = len(psdu) + 2
    if phr > 127:
        raise ValueError("frame exceeds recovered seven-bit PHR ceiling")
    return bytes([phr]) + psdu + struct.pack(">H", crc16_radio(psdu))


@dataclass
class Peer:
    source_eui: bytes
    serial: bytes
    slot: int
    state: str = "pending"
    key: bytes = bytes(16)
    rx_expected: int = 0
    tx_counter: int = 0
    expires_ms: int = PENDING_LIFETIME_MS
    joined_at_ms: int | None = None


@dataclass(frozen=True)
class Transition:
    event: str
    outcome: str
    evidence_class: str
    effects: tuple[str, ...] = ()


@dataclass(frozen=True)
class ParsedFrame:
    source_eui: bytes
    sequence: int
    vcmp_type: int
    flags: int
    body: bytes


def parse_plaintext_frame(frame: bytes) -> ParsedFrame:
    if len(frame) < 22:
        raise FrameRejected("truncated-vmac-frame")
    if frame[0] != len(frame) - 1 or frame[0] > 127:
        raise FrameRejected("phr-length-mismatch")
    psdu = frame[1:-2]
    if struct.unpack(">H", frame[-2:])[0] != crc16_radio(psdu):
        raise FrameRejected("radio-fcs-mismatch")
    if frame[1:3] != b"\x41\xc8":
        raise FrameRejected("unsupported-fcf")
    if frame[17] != 0:
        raise FrameRejected("protected-vcmp-not-modeled")
    body = frame[20:-2]
    received_crc = struct.unpack(">H", frame[18:20])[0]
    if received_crc != crc16_ccitt(frame[16:18] + b"\x00\x00" + body):
        raise FrameRejected("vcmp-crc-mismatch")
    return ParsedFrame(frame[8:16], frame[3], frame[16], frame[17], body)


@dataclass
class Efr32Model:
    peers: dict[bytes, Peer] = field(default_factory=dict)
    next_slot: int = 0
    free_slots: list[int] = field(default_factory=list)
    scan_active: bool = False
    saved_channel: int = 0
    live_channel: int = 0
    configured_channel: int = 0
    trace: list[Transition] = field(default_factory=list)

    def _record(self, event: str, outcome: str, *effects: str,
                evidence_class: str = "independently-reconstructed") -> Transition:
        transition = Transition(event, outcome, evidence_class, tuple(effects))
        self.trace.append(transition)
        return transition

    def _take_slot(self) -> int | None:
        if self.free_slots:
            return self.free_slots.pop(0)
        if self.next_slot < PEER_CAPACITY:
            slot = self.next_slot
            self.next_slot += 1
            return slot
        return None

    def _release(self, source: bytes) -> None:
        peer = self.peers.pop(source)
        self.free_slots.append(peer.slot)
        self.free_slots.sort()

    def process_join(self, source: bytes, serial: bytes, now_ms: int = 0) -> Transition:
        if len(source) != 8 or not serial or len(serial) > 0xFF:
            raise FrameRejected("invalid-join-fields")
        existing = self.peers.get(source)
        if existing is not None:
            slot = existing.slot
            self.peers[source] = Peer(source, bytes(serial), slot,
                                      expires_ms=now_ms + PENDING_LIFETIME_MS)
            return self._record("join", "reinitialized", "existing record zeroed except list links", "state=pending")

        slot = self._take_slot()
        if slot is None:
            return self._record("join", "pool-full", "input rejected before culling")
        self.peers[source] = Peer(source, bytes(serial), slot,
                                  expires_ms=now_ms + PENDING_LIFETIME_MS)

        if len(self.peers) == PEER_CAPACITY:
            joined = [peer for peer in self.peers.values() if peer.state == "joined"]
            if joined:
                victim = min(joined, key=lambda peer: (peer.expires_ms, peer.slot))
                self._release(victim.source_eui)
                return self._record("join", "allocated-and-culled-lru",
                                    f"allocated slot {slot}",
                                    f"evicted {victim.source_eui.hex()}")
            return self._record("join", "allocated-no-cull-all-pending",
                                f"allocated slot {slot}")
        return self._record("join", "allocated", f"allocated slot {slot}", "state=pending")

    def mark_joined(self, source: bytes, key: bytes, now_ms: int,
                    rx_expected: int = 0, tx_counter: int = 0) -> Transition:
        if len(key) != 16 or source not in self.peers:
            raise ValueError("known peer and 16-byte key required")
        peer = self.peers[source]
        peer.state = "joined"
        peer.key = bytes(key)
        peer.rx_expected = rx_expected & 0xFFFF
        peer.tx_counter = tx_counter & 0xFFFF
        peer.joined_at_ms = now_ms
        peer.expires_ms = now_ms + JOINED_LIFETIME_MS
        return self._record("permit-join", "joined", "key installed", "four-hour expiry")

    def expire(self, now_ms: int) -> list[str]:
        expired = sorted(
            (peer for peer in self.peers.values() if peer.expires_ms <= now_ms),
            key=lambda peer: peer.slot,
        )
        for peer in expired:
            self._release(peer.source_eui)
            self._record("timer", "peer-expired", peer.source_eui.hex())
        return [peer.source_eui.hex() for peer in expired]

    def accept_receive_counter(self, source: bytes, counter: int) -> bool:
        peer = self.peers.get(source)
        if peer is None:
            return False
        counter &= 0xFFFF
        distance = (counter - peer.rx_expected) & 0xFFFF
        if distance > REPLAY_AHEAD:
            self._record("protected-receive", "counter-rejected", f"distance={distance}")
            return False
        peer.rx_expected = (counter + 1) & 0xFFFF
        self._record("protected-receive", "counter-accepted",
                     f"rx_expected={peer.rx_expected}")
        return True

    def process_frame(self, frame: bytes, now_ms: int = 0) -> Transition:
        parsed = parse_plaintext_frame(frame)
        if parsed.vcmp_type == 7:
            body = parsed.body
            if len(body) < 26 or body[0] not in (4, 5):
                raise FrameRejected("unsupported-or-truncated-join")
            serial_length = body[14]
            if serial_length == 0 or len(body) != 25 + serial_length:
                raise FrameRejected("join-length-mismatch")
            return self.process_join(parsed.source_eui, body[15:15 + serial_length], now_ms)
        if parsed.vcmp_type == 4:
            if len(parsed.body) != 3 or parsed.body[0] != 1:
                raise FrameRejected("counter-sync-length-or-subtype")
            peer = self.peers.get(parsed.source_eui)
            if peer is None:
                return self._record("counter-sync", "unknown-peer")
            peer.tx_counter = struct.unpack(">H", parsed.body[1:])[0]
            return self._record("counter-sync", "counter-updated",
                                f"tx_counter={peer.tx_counter}")
        if parsed.vcmp_type == 11:
            if len(parsed.body) != 5:
                raise FrameRejected("scan-info-length")
            channel = struct.unpack(">H", parsed.body[:2])[0]
            regulatory = parsed.body[2]
            ranges = {1: range(0, 21), 2: range(21, 24),
                      3: range(24, 32), 4: range(100, 113)}
            if regulatory not in ranges or channel not in ranges[regulatory]:
                raise FrameRejected("unsupported-channel-or-region")
            return self._record(
                "scan-info",
                "scan-info-observed",
                f"reported_channel={channel}",
                f"regulatory_code={regulatory}",
                evidence_class="statically-recovered",
            )
        raise FrameRejected("outer-type-not-modeled")
