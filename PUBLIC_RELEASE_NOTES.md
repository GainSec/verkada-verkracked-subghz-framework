# Public release notes

This is a sanitized, standalone public-release candidate for the BH-series
Sub-GHz interoperability framework created by Jon “GainSec” Gaines.

## Included

- C++20 framing, CRC, VMAC, VCMP, message, and session implementation
- analytical DSSS/OQPSK acquisition, modulation, and demodulation
- raw IQ and SigMF file input plus passive evidence output
- native HackRF and UHD/B210 RX/TX plus RTL-SDR receive-only support
- guarded normal transmission and ordered-frame workflows
- persistent synthetic P-256 sensor identities, normal join, intended event,
  and fixed read-only coordinator RPC workflows over B210 full duplex
- isolation preflight, sensor retirement evidence, sensor-state simulation,
  shared RF/cloud events, PCAP/Wireshark export,
  coverage records, and coherent two-channel B210 measurements
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
- fuzzing, mutation, replay, counter manipulation, malformed acknowledgement
  modes, shell execution, and persistent-access workflows
- private research runs and their captures, device identities, network values,
  paths, credentials, session secrets, and forensic output

BH61 is the primary validated family. BH31 remains expected but unverified.
The retained fixture is analytical and explicitly `hardware_exact=false`.
Framing and FCS behavior were validated on owner-controlled hardware without
publishing live identifiers or captures. No vendor-production network
integration is included.

The public v2 stateful workflow is for isolated, owner-controlled laboratories.
It creates fresh synthetic identities and stores the private scalar only in an
owner-readable `sensor-private.key`. The repository includes no production
identity, RF key, or private laboratory evidence.

## Release verification

The v2 release candidate was rebuilt from a clean, non-local clone. All 19
native test executables and all 16 Python model, provenance, and public-safety
tests passed. The companion cloud emulator also completed its loopback-only
public integration workflow with this checkout.
