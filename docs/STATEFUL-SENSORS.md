# Stateful synthetic sensors

This guide describes the public v2 synthetic-sensor workflow. Use it only with
hardware that you own or are authorized to test. Keep the hub and the emulator
off vendor-production networks for the full run.

## What the workflow does

The framework creates a fresh P-256 sensor identity and persists its session.
The B210 runner can then perform these normal protocol actions:

1. Send a type-7 join request.
2. receive and validate the coordinator join response and signature;
3. derive the session key and persist the joined state;
4. send an intended sensor event and receive its normal acknowledgement; or
5. send one fixed read-only coordinator request and validate its response.

The six lifecycle and exchange commands are `sensor-create`, `sensor-status`,
`sensor-join`, `sensor-event`, `sensor-rpc-read`, and `sensor-retire`.

The runner records exact frames, waveforms, received material, state changes,
and SHA-256 manifests. It does not infer device acceptance from a host-side SDR
send. A join requires the expected response pair. An event requires the normal
acknowledgement. A read requires the matching response identifier and
transaction value.

## Safety boundary

The public workflow has these enforced limits:

- B210 is the only stateful hardware backend.
- The operator must supply the exact B210 serial and FPGA image.
- The sample rate is 4 to 20 Msps.
- The center frequency is inside 902–928 MHz.
- Receive timeout is 100 to 20,000 ms.
- TX gain is at most 85 dB; start at 0 dB.
- Each action requires exactly one of `--dry-run` or `--enable-tx`.
- A schema-v1 isolation preflight with `overall: pass` is mandatory.
- Only the listed models, events, and read operations are accepted.

There is no public fuzzing, mutation, replay, counter manipulation, malformed
acknowledgement mode, arbitrary RPC constructor, or shell-execution workflow.

## Session files

`sensor-create` creates a new directory and refuses to overwrite it.

| File | Purpose | Handling |
|---|---|---|
| `session.json` | Public identity and protocol state | Keep with the session |
| `sensor-private.key` | P-256 private scalar | Mode 0600; never log, export, or commit |
| `cloud-registration.json` | Public registration values | Import only into the owner-controlled emulator |
| `retired.json` | Local retirement marker | Created by `sensor-retire` |

The CLI output and registration JSON do not contain the private scalar or the
derived session key. Status output contains only identity, lifecycle state,
counters, retirement state, and a session-key fingerprint.

## Supported models

- `door-contact`
- `glass-break`
- `motion`
- `panic`
- `water`
- `wireless-relay`

## Intended event names

- `tamper`
- `water-dry`, `water-wet`
- `motion`
- `contact-closed`, `contact-opened`
- `glass-break`
- `heartbeat`, `supervision`, `heartbeat-secondary`
- `panic-pressed`
- `relay-input-closed`, `relay-input-opened`
- `reverse-contact-opened`, `reverse-contact-closed`

Each event requires an operator-supplied 32-bit `--event-id`. Use a new value
for each intended event in one session.

## Read-only coordinator operations

| `--rpc` value | Additional option | Limit |
|---|---|---|
| `echo` | `--payload-hex HEX` | 1–64 bytes |
| `image-state` | none | fixed empty request |
| `range-status` | none | fixed empty request |
| `config` | `--config-id N` | 16-bit identifier |
| `statistics` | `--stat-group N` | 0–11 |
| `peer` | `--index N` | 8-bit index |
| `mempool` | `--index N` | 8-bit index |

The CLI cannot specify a raw request identifier or arbitrary request body.

## Recommended sequence

1. Build with UHD and TX enabled.
2. Record the hub, B210, FPGA, antenna, attenuation, and channel.
3. Start the local cloud emulator.
4. establish local DNS and block vendor-network egress;
5. start a UART health oracle if the lab uses one;
6. run `bh61_isolation_preflight.sh` and inspect every gate;
7. create a synthetic session;
8. perform `sensor-join` first with `--dry-run`;
9. repeat the join with `--enable-tx` and TX gain 0;
10. inspect `sensor-status` and the join evidence;
11. send one intended event;
12. make one read-only request;
13. correlate RF evidence with the local emulator journal; and
14. retire the session when the run ends.

Use a new output directory for every action. Evidence writers refuse to
overwrite prior results.

## Lifecycle commands

```sh
./build-native/bh61-radio sensor-status \
  --session sessions/synthetic-door-01 --jsonl

./build-native/bh61-radio sensor-retire \
  --session sessions/synthetic-door-01 \
  --output runs/synthetic-door-01-retirement
```

Retirement blocks further stateful actions from that local session. The
retirement record states that matching cloud-side retirement is still required.
The command does not delete the private key; preserve or destroy it according
to the laboratory's evidence-retention policy.

## Result language

Report each layer separately:

- host generated the frame;
- SDR accepted the waveform;
- an independent receiver observed RF;
- hub returned a matching response;
- session state advanced;
- emulator journal recorded the correlated action; and
- an owner-visible state change occurred.

Do not combine these statements into “worked” unless the evidence supports
each required layer.
