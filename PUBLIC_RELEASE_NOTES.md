# Public release notes

This is a sanitized, standalone public-release candidate for the BH-series
Sub-GHz interoperability framework created by Jon “GainSec” Gaines.

## Included

- C++20 framing, CRC, VMAC, VCMP, message, and session implementation
- analytical DSSS/OQPSK acquisition, modulation, and demodulation
- raw IQ and SigMF file input plus passive evidence output
- optional HackRF enumeration and RX library implementation
- portable C++ tests and synthetic EFR32 behavioral-model tests
- independently reconstructed protocol fixtures and analytical PHY fixtures
- public architecture, provenance, fixture, hardware, and methodology docs

## Intentionally excluded

- the complete private evidence corpus and workbench metadata
- proprietary firmware, extracted binaries, root filesystems, archives, and
  vendor-derived i.MX7 input files
- live device credentials, RF/session keys, identifiers, and customer data
- private RF captures, screenshots, shell transcripts, and lab output
- cloud-emulator code, which is a separate project
- vulnerability-specific EFR32 behavior, exploit fixtures, security reports,
  vendor correspondence, and embargoed findings

BH61 is modeled but public physical validation is not claimed. BH31 remains
expected but unverified. The retained waveform is analytical and explicitly
`hardware_exact=false`. Native HackRF capture is implemented at the library
layer but not yet connected to the public `capture` CLI command. There is no RF
transmit command and no vendor-production network integration.
