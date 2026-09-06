# Passive evidence fixture contract

The framework accepts two receive-only evidence forms:

1. A SigMF `cf32_le` metadata/data pair plus decoded-event JSONL. The data size must be a whole number of complex float samples. The stored sample count must match the file.
2. Standalone decoded-frame JSONL. Each row must include a complete FCS-valid BH61 `frame_hex`, sample rate, and center frequency. UTC, monotonic time, and RSSI are retained when supplied.

Ingestion is atomic. One malformed row rejects the full input. It produces no
partial normalized result. A valid row is classified by outer VCMP type, such
as `type7-join`, `counter-sync`, or `scan-info-rx`. This is parser output only;
it does not claim that a physical EFR32 accepted the frame.

The manifest points to the independently generated analytical type-11 fixture
under `fixtures/phy`. No RF capture is represented as observed or live evidence.
