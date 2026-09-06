# Standalone Sub-GHz Public Release Design

## Objective

Create a fresh-history, standalone public repository for the independently
authored BH-series Sub-GHz interoperability framework. Credit Jon “GainSec”
Gaines while excluding private evidence, vendor material, credentials, and
embargoed vulnerability research.

## Selection strategy

Use allowlist reconstruction. Retain reviewed C++20 core, DSP, radio,
passive-evidence, CLI, CMake, and portable tests. Review fixtures and research
files individually. Do not copy the source repository wholesale, its Git
metadata, `evidence/`, `cloud-emulator/`, private reports, or security plans.

This approach preserves more technical value than a minimal core-only release
and creates less disclosure risk than copying the private tree and pruning it.

## Public architecture

The executable data path remains:

```text
RF/IQ input (file, SigMF, or optional HackRF RX)
  -> radio abstraction
  -> acquisition and synchronization
  -> analytical DSSS/OQPSK demodulation
  -> framing and CRC
  -> VCMP/VMAC/session parsing
  -> normalized passive evidence output
```

Offline encoders and the analytical modulator remain for deterministic tests.
They do not constitute SDR transmit support or validated over-the-air control.

## Research and fixtures

Retain independently authored EFR32 behavioral models only when they can run
from synthetic/reconstructed inputs. Exclude exact-evidence importers and any
corpus whose provenance cannot be established safely. Retain i.MX7 work only
as sanitized interface summaries; extracted device-tree, kernel, bootloader,
and root-filesystem inputs are vendor-derived and excluded.

Every retained fixture receives a public provenance classification. Analytical
waveforms keep `hardware_exact=false` unless explicit physical evidence proves
otherwise. Captured/private fixtures are excluded by default.

## Documentation and safety

Rewrite the README and add architecture, provenance, fixtures, methodology,
hardware support, security, contribution, release-note, authorship, and
license-status documents. The project is explicitly passive/RX-focused. It
must not contact vendor infrastructure or contain credentials, firmware,
customer data, security findings, or production-targeting defaults.

## Validation and release

Configure and build in a directory outside the public tree, run CTest and safe
Python research tests, inspect every opaque file, scan content and filenames,
and review staged Git content. Only then initialize Git and make one root
commit. Do not configure a remote or push.
