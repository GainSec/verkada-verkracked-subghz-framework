#!/usr/bin/env python3
"""Generate the recovered EFR32 Custom_OQPSK mode-2 waveform."""

import argparse
import json

import numpy as np
from scipy.signal import resample_poly


DSSS_WORDS = (
    0xC8DD7892, 0x8DD7892C, 0xDD7892C8, 0xD7892C8D,
    0x7892C8DD, 0x892C8DD7, 0x92C8DD78, 0x2C8DD789,
    0x6277D238, 0x277D2386, 0x77D23862, 0x7D238627,
    0xD2386277, 0x2386277D, 0x386277D2, 0x86277D23,
)
MODE2_TAPS = np.array(
    [1, 1, 16, 48, 80, 112, 127, 127,
     127, 127, 112, 80, 48, 16, 1, 1],
    dtype=np.float64,
) / 128.0


def generate(frame: bytes, sample_rate: int) -> np.ndarray:
    if sample_rate <= 0:
        raise ValueError("sample rate must be positive")
    symbols = [0] * 10 + [0, 7, 10]
    symbols.extend(nibble for byte in frame for nibble in (byte & 0xF, byte >> 4))
    chips = np.array([
        1.0 - 2.0 * ((DSSS_WORDS[symbol] >> bit) & 1)
        for symbol in symbols for bit in range(32)
    ])

    # The EFR32 O-QPSK path is a continuous-phase representation. Each chip
    # interval rotates by +/- pi/2. Its sign is derived from adjacent chips and
    # alternates with I/Q parity. The mode-2 FIR operates at 8x chip rate.
    prior = 1.0 - 2.0 * ((DSSS_WORDS[0] >> 31) & 1)
    direction = np.empty(chips.size)
    direction[0] = -chips[0] * prior
    indices = np.arange(1, chips.size)
    direction[1:] = ((-1.0) ** (indices + 1)) * chips[1:] * chips[:-1]
    impulses = np.zeros(direction.size * 8)
    impulses[::8] = direction
    shaped = np.convolve(impulses, MODE2_TAPS)[:impulses.size]
    phase = np.cumsum(shaped) * (np.pi / 16.0)
    internal = np.exp(1j * phase)

    # The exact shaping clock is 8 * 640 kchip/s = 5.12 MHz. Polyphase
    # resampling preserves phase continuity at the SDR rate.
    divisor = np.gcd(sample_rate, 5_120_000)
    output = resample_poly(
        internal, sample_rate // divisor, 5_120_000 // divisor
    )
    expected = round(chips.size * sample_rate / 640_000)
    return output[:expected].astype(np.complex64)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--frame-hex", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--sample-rate", type=int, default=4_000_000)
    parser.add_argument("--amplitude", type=float, default=0.5)
    parser.add_argument("--prefix-samples", type=int, default=20_000)
    parser.add_argument("--suffix-samples", type=int, default=8_000)
    args = parser.parse_args()
    frame = bytes.fromhex(args.frame_hex)
    if not 0.0 < args.amplitude <= 0.9:
        raise ValueError("amplitude must be in (0, 0.9]")
    active = generate(frame, args.sample_rate) * np.float32(args.amplitude)
    output = np.concatenate((
        np.zeros(args.prefix_samples, dtype=np.complex64), active,
        np.zeros(args.suffix_samples, dtype=np.complex64),
    ))
    output.tofile(args.output)
    print(json.dumps({
        "frame_hex": frame.hex(), "sample_rate": args.sample_rate,
        "active_samples": int(active.size), "output_samples": int(output.size),
        "peak": float(np.max(np.abs(output))),
        "shaping_mode": 2,
        "taps": [1, 1, 16, 48, 80, 112, 127, 127,
                 127, 127, 112, 80, 48, 16, 1, 1],
    }, sort_keys=True))


if __name__ == "__main__":
    main()
