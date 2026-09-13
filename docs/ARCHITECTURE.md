# Architecture

The framework keeps acquisition, signal processing, protocol parsing, and
evidence output separate so each boundary can be tested without radio hardware.

```text
RF/IQ input
  |-- cf32_le file
  |-- SigMF metadata/data pair
  |-- HackRF RX/TX
  |-- UHD/B210 RX/TX and coherent dual RX
  `-- RTL-SDR RX
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
VMAC -> VCMP -> typed messages / persistent sensor session
          |
          v
normal join / intended event / read-only coordinator request runner
          |
          v
normalized JSONL, SigMF, PCAP, and operation evidence writer
```

## Components

- `include/bh61/core`, `src/core`: byte utilities, CRC domains, radio frame,
  VMAC addressing, VCMP envelopes, typed messages, and session state.
- `include/bh61/dsp`, `src/dsp`: profiles, DSSS mapping, analytical modulator,
  acquisition, and demodulation.
- `include/bh61/radio`, `src/radio`: common device abstraction, deterministic
  file backend, HackRF, UHD/B210, and RTL-SDR adapters.
- `include/bh61/evidence`, `src/evidence`: atomic passive importer and writer
  for SigMF, JSONL, discontinuity records, and SHA-256 manifests.
- `src/apps`: decode, capture, generation, guarded normal TX, synthetic sensor
  lifecycle, stateful full-duplex exchanges, simulation, PCAP, coverage, and
  coherent-measurement CLI operations.
- `research/efr32`: bounded offline behavioral model and generated synthetic
  parser corpus.

## Data and error boundaries

Parsers return structured failures for truncation, length, unsupported address
forms, and checksum mismatch. Passive import rejects a malformed transaction
without returning partial normalized rows. Hardware RX reports timeout,
end-of-stream, cancellation, removal, and transport errors separately; queue
overflow is recorded as a discontinuity.

Offline encoders and modulation are deterministic test tools. Native transmit
paths are compiled only when enabled and require a second runtime
acknowledgement. Each operation applies bounded duration, frequency, gain,
and frequency policies and records the exact waveform hash. Stateful sensor
commands additionally require an exact B210 identity, an FPGA image, an
isolation-preflight result, a bounded receive timeout, and exactly one of
`--dry-run` or `--enable-tx`.

The sensor runner accepts only normal protocol behavior. It has no public
counter-resynchronization diagnostic, malformed acknowledgement mode,
arbitrary RPC constructor, replay engine, mutation engine, or fuzzing surface.
