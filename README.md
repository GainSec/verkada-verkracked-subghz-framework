## Verkracked Part 5 - Verkada alarm hub Sub-GHz interoperability framework

This repo is part of a an ongoing independent security research project informally named: "Verkracked". There is a lot more to come as this is considered Part 5 with Parts 1-3 pending their responsible full disclosure windows (12/05/2026).

Part 4 is the cloud emulator allowing owners of the alarm hub to use their legally owned devices locally. Found [HERE](https://github.com/gainsec/verkada-verkracked-alarm-hub-local-framework)

Part 0, introducing this independent security reearch project can be found [HERE](https://gainsec.com/2026/09/06/verkracked-security-research-on-verkada-anti-crime-devices-part-0/)

This project is an independent Sub-GHz interoperability and research framework
for lawfully acquired BH-series hardware. It provides independently authored
tooling for passive RF acquisition, protocol decoding, framing analysis, PHY
modeling, and reproducible laboratory research.

The project is intended for owner controlled hardware, interoperability,
repair, preservation, and security research conducted on systems users are
authorized to operate.

The repository does not distribute Verkada firmware, device credentials,
customer data, private RF captures, or unpublished embargoed vulnerability
findings. Verkada is a trademark of its owner. This project is independent and
is not affiliated with or endorsed by Verkada.

## What it does

The framework provides a C++20 library and `bh61-radio` command-line tool for:

- parsing and rebuilding recovered radio framing, CRC, VMAC, VCMP, message,
  and session structures;
- generating frames and deterministic waveforms entirely offline;
- analytical DSSS and half-sine OQPSK modulation/demodulation;
- finite-hypothesis acquisition, synchronization, and impaired-signal tests;
- decoding raw `cf32_le` IQ files and SigMF data/metadata pairs;
- normalizing passive captures into SigMF, JSONL, reproduction commands, and
  SHA-256 manifests;
- enumerating an optional native HackRF backend and exercising its RX library;
  and
- running an offline EFR32 parser/peer-state behavioral model against a
  deterministic synthetic corpus.

The public command line is receive/offline focused. It exposes no RF transmit
command. Frame construction and the file-device transmit interface exist for
offline tests and analytical modeling; they are not validated over-the-air
injection or device-control capabilities.

## Supported devices and required hardware

The primary target is the **Verkada BH61 Wireless Alarm Hub**, a PoE-powered
Classic Alarms hub that communicates with wireless alarm peripherals over a
regional Sub-GHz link. See Verkada's [BH61 installation guide][bh61-guide],
[wireless-device setup guide][wireless-setup], and [wireless intrusion product
overview][wireless-overview] for the manufacturer's hardware documentation.
This project models the hub-side framing, protocol, DSP, and EFR32 behavior; it
does not replace the manufacturer's installation or life-safety guidance.

The associated wireless-device family documented for the BH61 includes the
[BR31 door sensor][br31-guide], [BR32 motion sensor][br32-guide], [BR33 panic
button][br33-guide], [BR34 glass-break sensor][br34-guide], [BR35 water-leak
sensor][br35-guide], and [BX21 wireless relay][bx21-guide]. These links identify
the devices whose traffic may be encountered in an authorized BH61 lab. They
do not mean every peripheral has been individually tested with this project.
BH31 remains an experimental compatibility target without a BH31-specific
public capture or physical validation.

For offline work, only a computer with CMake 3.24+, a C++20 compiler, and the
checked-in synthetic fixtures is required. Raw `cf32_le` IQ and SigMF files can
be inspected without any radio attached. Passive live-RF research additionally
requires:

- a **HackRF One or HackRF Pro** with libhackrf development files installed;
- a receive antenna suitable for the hardware's regional band (the vendor
  documents 915 MHz for the US/Canada and 868 MHz for the UK/EU);
- a data-capable USB connection and a computer able to sustain IQ capture; and
- lawfully owned BH61 hardware and peripherals when performing physical
  validation rather than fixture-only analysis.

The native HackRF RX transport and device enumeration are implemented, but the
`capture` command is not yet wired directly to an attached HackRF. The current
end-to-end CLI workflow analyzes operator-supplied raw `cf32_le` or SigMF input.
No transmit-capable backend or RF transmit command is included.

[bh61-guide]: https://docs.verkada.com/docs/wireless-alarm-hub-install-guide.pdf
[wireless-setup]: https://help.verkada.com/classic-alarms/installation/alarm-setup-and-install-best-practices/set-up-your-wireless-alarm-devices
[wireless-overview]: https://docs.verkada.com/docs/wireless-intrusion-overview.pdf
[br31-guide]: https://docs.verkada.com/docs/br31-quick-start-guide.pdf
[br32-guide]: https://docs.verkada.com/docs/br32-quick-start-guide.pdf
[br33-guide]: https://docs.verkada.com/docs/br33-quick-start-guide.pdf
[br34-guide]: https://docs.verkada.com/docs/br34-quick-start-guide.pdf
[br35-guide]: https://docs.verkada.com/docs/br35-quick-start-guide.pdf
[bx21-guide]: https://docs.verkada.com/docs/wireless-relay-bx21-datasheet.pdf

## Current limits

- BH61 is the primary modeled family. The public repository does not claim
  physical RF validation for the checked-in waveform or fixtures.
- BH31 behavior is expected but unverified; BH61 evidence must not be treated
  as BH31 confirmation.
- The waveform fixture is independently generated and
  `hardware_exact=false`. It is analytically modeled, not demonstrated as
  bit-for-bit or waveform-exact against physical BH hardware.
- Native HackRF enumeration, configuration, conversion, queuing, and RX
  transport are implemented. Direct HackRF-to-`capture` CLI selection is not
  wired yet; `capture --input` currently operates on files.
- No UHD/USRP or RTL-SDR backend is implemented.
- Unknown protocol fields remain opaque. Implementation does not prove a
  physical device accepts or emits a modeled structure.

See [hardware support](docs/HARDWARE-SUPPORT.md) and
[claim provenance](docs/PROVENANCE.md).

## Requirements

- CMake 3.24 or newer
- a C++20 compiler
- Python 3 for the EFR32 research tests and fixture generator
- optional libhackrf development headers/library for the native RX backend

No network service or vendor account is required. The tools contain no vendor
production endpoint and make no production API request.

## Build and test

Portable build without radio hardware:

```sh
cmake -S . -B build -DBH61_ENABLE_HARDWARE=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests/research -p 'test_*.py'
```

Optional HackRF RX library build:

```sh
cmake -S . -B build-hackrf \
  -DBH61_ENABLE_HARDWARE=ON \
  -DBH61_ENABLE_HACKRF=ON \
  -DBH61_REQUIRE_HACKRF=ON
cmake --build build-hackrf --parallel
ctest --test-dir build-hackrf --output-on-failure
./build-hackrf/bh61-radio devices --jsonl
```

The last command enumerates devices. It does not start reception or transmit.

## Work with safe sample data

Inspect the reconstructed type-11 frame:

```sh
frame=$(sed -n '/^[0-9a-f]/p' fixtures/protocol/type11-f27-normal-destination.hex)
./build/bh61-radio inspect --hex "$frame"
```

Generate and decode a temporary analytical waveform:

```sh
./build/bh61-radio waveform \
  --frame-hex "$frame" \
  --output /tmp/bh-series-analytical.cf32 \
  --sample-rate 4000000

./build/bh61-radio decode \
  --input /tmp/bh-series-analytical.cf32 \
  --sample-rate 4000000 \
  --jsonl
```

Normalize a user-supplied raw IQ file into a SigMF pair and hash manifest:

```sh
mkdir -p captures/private-lab
./build/bh61-radio capture \
  --input /path/outside/repository/owned-capture.cf32 \
  --output captures/private-lab \
  --stem owned-lab-rx \
  --sample-rate 4000000 \
  --center-frequency 915350000
```

The frequency above is a model/example value, not a universal live channel.
Determine lawful settings and the actual regional/device configuration in your
own lab. Keep generated captures outside Git; `captures/` is ignored.

To decode a SigMF pair, pass its metadata path:

```sh
./build/bh61-radio decode \
  --input captures/private-lab/owned-lab-rx.sigmf-meta \
  --jsonl
```

## Capture safety and contributions

Only acquire signals and hardware data you are authorized to study. Before
sharing any derived result, remove device-specific EUIs, PAN data, RF/session
keys, serials, receiver serials, timestamps, location metadata, and unrelated
traffic. Do not submit raw private captures. Prefer reproducible synthetic
vectors generated by this project.

Every fixture must meet [the fixture policy](docs/FIXTURES.md) and the
[contribution rules](CONTRIBUTING.md).

## Architecture and evidence discipline

- [Architecture](docs/ARCHITECTURE.md)
- [Provenance and claim status](docs/PROVENANCE.md)
- [Fixture policy and manifest](docs/FIXTURES.md)
- [Research methodology](docs/RESEARCH-METHODOLOGY.md)
- [EFR32 model](research/efr32/README.md)
- [i.MX7/EFR32 boundary](research/imx7/README.md)

## Author 

[Jon "GainSec" Gaines](https://gainsec.com)
