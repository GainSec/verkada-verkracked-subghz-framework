# Architecture

The framework keeps acquisition, signal processing, protocol parsing, and
evidence output separate so each boundary can be tested without radio hardware.

```text
RF/IQ input
  |-- cf32_le file
  |-- SigMF metadata/data pair
  `-- optional HackRF RX library
          |
          v
radio Device abstraction / FileDevice / HackrfDevice
          |
          v
finite-hypothesis acquisition and synchronization
          |
          v
analytical DSSS / half-sine OQPSK demodulation
          |
          v
radio framing and CCITT-derived FCS
          |
          v
VMAC -> VCMP -> typed messages / session model
          |
          v
normalized JSONL and SigMF evidence writer
```

## Components

- `include/bh61/core`, `src/core`: byte utilities, CRC domains, radio frame,
  VMAC addressing, VCMP envelopes, typed messages, and session state.
- `include/bh61/dsp`, `src/dsp`: profiles, DSSS mapping, analytical modulator,
  acquisition, and demodulation.
- `include/bh61/radio`, `src/radio`: common device abstraction, deterministic
  file backend, HackRF types, bounded RX queue, and optional libhackrf adapter.
- `include/bh61/evidence`, `src/evidence`: atomic passive importer and writer
  for SigMF, JSONL, discontinuity records, and SHA-256 manifests.
- `src/apps`: offline/receive-oriented CLI operations.
- `research/efr32`: bounded offline behavioral model and generated synthetic
  parser corpus.

## Data and error boundaries

Parsers return structured failures for truncation, length, unsupported address
forms, and checksum mismatch. Passive import rejects a malformed transaction
without returning partial normalized rows. Hardware RX reports timeout,
end-of-stream, cancellation, removal, and transport errors separately; queue
overflow is recorded as a discontinuity.

Offline encoders and modulation are deterministic test tools. The native
HackRF transport has no transmit implementation, and the CLI exposes no
transmit command. Native HackRF RX is not yet selected by `capture`; the current
CLI capture path uses a file device unless embedded by another owner-controlled
application.
