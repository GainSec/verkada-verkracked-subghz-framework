## Verkracked Part 5 - Verkada alarm hub Sub-GHz interoperability framework

This repo is part of a an ongoing independent security research project informally named: "Verkracked". There is a lot more to come as this is considered Part 5 with Parts 1-3 pending their responsible full disclosure windows (12/05/2026).

Part 4 is the cloud emulator allowing owners of the alarm hub to use their legally owned devices locally. Found [HERE](https://github.com/gainsec/verkada-verkracked-alarm-hub-local-framework)

Part 0, introducing this independent security reearch project can be found [HERE](https://gainsec.com/2026/09/06/verkracked-security-research-on-verkada-anti-crime-devices-part-0/)

This project is an independent Sub-GHz interoperability and research framework
for lawfully acquired BH-series hardware. It provides independently authored
tooling for RF acquisition, controlled transmission, protocol decoding, framing
analysis, PHY modeling, and reproducible owner-controlled laboratory research.

The project is intended for owner controlled hardware, interoperability,
repair, preservation, and security research conducted on systems users are
authorized to operate.

The repository does not distribute Verkada firmware, device credentials,
customer data, private RF captures, or unpublished embargoed vulnerability
findings. Verkada is a trademark of its owner. This project is independent and
is not affiliated with or endorsed by Verkada.

## What can I do with this framework and my alarm hub?

| Feature | What you can do | Hardware or input |
|---|---|---|
| Frame inspection | Parse recovered radio framing, CRC domains, VMAC, VCMP, messages, counters, and payloads | Frame bytes in hexadecimal |
| Frame construction | Build complete join, scan-complete, and protected protocol frames from fields | Synthetic field values, key, and IV |
| Waveform generation | Convert complete frames into the recovered DSSS and half-sine OQPSK waveform | Frame bytes; no radio required |
| IQ decoding | Acquire, synchronize, demodulate, and decode BH61 bursts | Raw `cf32_le`, SigMF, B210, HackRF, or RTL-SDR input |
| Capture evidence | Save normalized SigMF, JSONL, reproduction commands, timestamps, and SHA-256 manifests | Live receiver or operator-supplied IQ |
| Normal RF transmission | Send generated or supplied valid BH61 frames through a normal antenna port | B210 or HackRF with TX enabled |
| Ordered exchanges | Send up to 100 valid frames in one timed sequence with controlled intervals | B210, HackRF, or file backend |
| Synthetic sensor identity | Create and persist a fresh P-256 identity for a door, motion, panic, glass-break, water, or relay sensor | Local session directory |
| Synthetic sensor pairing | Perform the normal join exchange, validate both coordinator responses, and derive the session key | B210 plus an owned BH61 |
| Intended sensor activity | Send tamper, water, motion, contact, glass-break, heartbeat, panic, relay-input, and reverse-contact events and validate the normal acknowledgement | Joined synthetic sensor over B210 |
| Coordinator reads | Request echo, image state, range status, configuration, statistics, peer, and memory-pool data | Joined synthetic sensor over B210 |
| Sensor-state simulation | Model enrollment, contact, motion, tamper, battery, supervision, and retransmission state as correlated events | Scenario text file; no radio required |
| Wireshark export | Write DLT_USER0 PCAP and dissect PHR, VMAC, VCMP, payload, and FCS fields | Valid BH61 frame |
| RF and cloud correlation | Produce `bh61.lab.event/v1` records that import into the companion cloud emulator | Any capture or transmit workflow |
| Coherent measurement | Capture two synchronized B210 channels and calculate calibrated phase and amplitude observations | B210 with two receive channels |

Supported radio backends:

| Backend | Receive | Transmit | Notes |
|---|---:|---:|---|
| Ettus B210/UHD | Yes | Yes | Timed TX and coherent two-channel RX |
| HackRF/libhackrf | Yes | Yes | One channel; immediate TX only |
| RTL-SDR/librtlsdr | Yes | No | Receive-only by hardware design |
| File | Yes | Yes | Deterministic tests without RF hardware |

Transmission always requires `--enable-tx`, or `--dry-run` for validation
without RF. The `efr32-custom-oqpsk-mode2` waveform was accepted repeatedly by
a live BH61 on 2026-09-07. A successful host-side send still does not by itself
prove target acceptance; use target counters or a second receiver.

## Build and verify

```sh
sudo apt install build-essential cmake pkg-config libhackrf-dev hackrf libuhd-dev uhd-host librtlsdr-dev rtl-sdr
cmake -S . -B build-native \
  -DBH61_ENABLE_HARDWARE=ON \
  -DBH61_ENABLE_HACKRF=ON \
  -DBH61_REQUIRE_HACKRF=ON \
  -DBH61_ENABLE_UHD=ON \
  -DBH61_ENABLE_RTLSDR=ON \
  -DBH61_ENABLE_TX=ON
cmake --build build-native -j4
ctest --test-dir build-native --output-on-failure
./build-native/bh61-radio devices --jsonl
```

## Main commands

```sh
# Inspect and generate a known recovered frame.
./build-native/bh61-radio inspect --hex 1a41c800ff01010000000000000000000b00338b0000010000705c
./build-native/bh61-radio waveform \
  --frame-hex 1a41c800ff01010000000000000000000b00338b0000010000705c \
  --output /tmp/bh61.cf32 --sample-rate 4000000

# Build complete type-7, type-11, and protected frames from fields.
join=$(./build-native/bh61-radio join-frame \
  --source-eui 1112131415161718 --destination 1 --pan 511 \
  --sequence 1 --serial-hex 58)
scan=$(./build-native/bh61-radio scan-complete-frame \
  --source-eui 2122232425262728 --destination 1 --pan 511 \
  --sequence 2 --channel 0 --regulatory-code 1 --scan-sequence 10)
secure=$(./build-native/bh61-radio secure-frame \
  --source-eui 3132333435363738 --destination 1 --pan 511 \
  --sequence 3 --type 1 --flags 1 --counter 4660 --payload-hex 00 \
  --key-hex 30313233343536373839414243444546 \
  --iv-hex 000102030405060708090a0b0c0d0e0f)

# Send an ordered frame sequence through one device open and one RF burst.
./build-native/bh61-radio transmit --backend b210 --serial B210_SERIAL \
  --frame-hex "$join,$secure" --output captures/private-lab/sequence-001 \
  --stem sequence-001 --sample-rate 4000000 \
  --center-frequency 915000000 --tx-gain 0 --interval-ms 10 --enable-tx

# Transmit through a HackRF at the normal antenna port.
./build-native/bh61-radio transmit --backend hackrf --serial HACKRF_SERIAL \
  --frame-hex 1a41c800ff01010000000000000000000b00338b0000010000705c \
  --output captures/private-lab/tx-001 --stem tx-001 --sample-rate 4000000 \
  --center-frequency 915350000 --tx-gain 0 \
  --utc 2026-09-06T18:00:00Z --enable-tx

# Schedule a B210 transmission in UHD device time.
./build-native/bh61-radio transmit --backend uhd --serial B210_SERIAL \
  --input /tmp/bh61.cf32 --output captures/private-lab/timed-001 --stem timed-001 \
  --sample-rate 4000000 --center-frequency 915350000 --tx-gain 0 \
  --at-ns 5000000000 --enable-tx

# Capture two coherent B210 channels and calculate calibrated phase/amplitude.
./build-native/bh61-radio df-capture --backend uhd --serial B210_SERIAL \
  --output captures/private-lab/df-001 --stem df-001 --sample-count 4000000 \
  --sample-rate 4000000 --center-frequency 915350000 \
  --antenna two-element-linear --calibration-phase-rad 0.0 \
  --utc 2026-09-06T18:00:00Z
```

Start with zero TX gain and increase it only when the measured laboratory link
requires it. The CLI restricts this recovered profile to 902–928 MHz, one
waveform to 10 seconds, one command to 60 seconds of aggregate airtime, and
HackRF TX gain to 0–47 dB. A comma-separated `--frame-hex` sequence is limited to 100 frames and
is emitted as one waveform with the requested silence interval between frames.
These software bounds do not replace local RF rules or safe lab setup.

## Stateful synthetic sensors

The B210 full-duplex path can create an owner-controlled synthetic identity,
perform the normal join exchange, send one of the recovered intended event
semantics, and issue a fixed allowlist of read-only coordinator requests.
Start with a dry run and the isolation preflight. The private P-256 scalar is
stored only as `sensor-private.key` with owner-only permissions. Do not copy it
into logs, support bundles, evidence directories, or Git.

```sh
./build-native/bh61-radio sensor-create \
  --session sessions/synthetic-door-01 \
  --serial SYN-DOOR-0001 --model door-contact

tools/bh61_isolation_preflight.sh \
  --output preflight.json --hub-ip HUB_LAB_IP --hub-mac HUB_LAB_MAC \
  --dns-ip LAB_DNS_IP --domains-file lab-domains.txt \
  --emulator-health-url http://LAB_EMULATOR_HOST:PORT/healthz \
  --router-host LAB_ROUTER --radio-host local \
  --b210-serial B210_SERIAL --fpga usrp_b210_fpga.bin \
  --fpga-sha256 FPGA_SHA256 --uart-oracle uart-health.txt

./build-native/bh61-radio sensor-join \
  --session sessions/synthetic-door-01 --output runs/join-dry \
  --preflight preflight.json --backend b210 --serial B210_SERIAL \
  --fpga usrp_b210_fpga.bin --sample-rate 4000000 \
  --center-frequency 915350000 --rx-timeout-ms 2000 --dry-run

# Replace --dry-run with --enable-tx only after every preflight gate passes.
./build-native/bh61-radio sensor-event \
  --session sessions/synthetic-door-01 --output runs/contact-opened \
  --preflight preflight.json --backend b210 --serial B210_SERIAL \
  --fpga usrp_b210_fpga.bin --sample-rate 4000000 \
  --center-frequency 915350000 --rx-timeout-ms 2000 --tx-gain 0 \
  --event contact-opened --event-id 1 --enable-tx

./build-native/bh61-radio sensor-rpc-read \
  --session sessions/synthetic-door-01 --output runs/image-state \
  --preflight preflight.json --backend b210 --serial B210_SERIAL \
  --fpga usrp_b210_fpga.bin --sample-rate 4000000 \
  --center-frequency 915350000 --rx-timeout-ms 2000 --tx-gain 0 \
  --rpc image-state --enable-tx
```

See [Stateful synthetic sensors](docs/STATEFUL-SENSORS.md) for all models,
events, read operations, lifecycle commands, and acceptance evidence.

## Evidence and cloud correlation

TX creates a JSON record, correlated `bh61.lab.event/v1` JSONL, the exact CF32
waveform, and a SHA-256 manifest. `df-capture` creates both channel files, a
calibrated observation, and a manifest. Import event streams into the cloud
emulator with:

```sh
bh61-cloud-emulator lab-event --token "$ADMIN_TOKEN" ingest captures/private-lab/tx-001/tx-001.events.jsonl
```

The full operator sequence is in
[Integrated RF and cloud lab runbook](docs/INTEGRATED-RF-CLOUD-LAB.md).

## Wireshark

Export a decoded frame and install the Lua dissector:

```sh
./build-native/bh61-radio pcap \
  --hex 1a41c800ff01010000000000000000000b00338b0000010000705c \
  --output bh61.pcap
mkdir -p "$HOME/.local/lib/wireshark/plugins"
cp tools/wireshark/bh61.lua "$HOME/.local/lib/wireshark/plugins/bh61.lua"
wireshark bh61.pcap
```

The dissector handles the framework's DLT_USER0 records and exposes PHR,
VMAC, VCMP, payload, received FCS, and computed FCS validity.

## Public release boundary

This repository contains no vendor firmware, credentials, private captures,
live device identifiers, customer data, persistent-access mechanism, turnkey
root-execution chain, shell execution, fuzzing, mutation, replay, counter
manipulation, malformed acknowledgement modes, or embargoed vulnerability
report. Operators supply their own identifiers and captures at runtime. The
file backend supports deterministic use without hardware.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Hardware and backend support](docs/HARDWARE-SUPPORT.md)
- [Stateful synthetic sensors](docs/STATEFUL-SENSORS.md)
- [Integrated RF and cloud laboratory](docs/INTEGRATED-RF-CLOUD-LAB.md)
- [Provenance and claim status](docs/PROVENANCE.md)
- [Fixture policy](docs/FIXTURES.md)
- [Research methodology](docs/RESEARCH-METHODOLOGY.md)
- [EFR32 model](research/efr32/README.md)
- [i.MX7/EFR32 boundary](research/imx7/README.md)

## Author

[Jon "GainSec" Gaines](https://gainsec.com)
