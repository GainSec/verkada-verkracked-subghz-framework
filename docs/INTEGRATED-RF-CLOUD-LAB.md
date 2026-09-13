# BH61 integrated RF and cloud lab runbook

Use this runbook only with owned BH61 hardware in the isolated laboratory. It
is written for repeatable evidence collection. Replace every uppercase value
before you run a command.

## 1. Record the test identity

Create one correlation stem. Use it for all RF and cloud artifacts from the
same action.

```sh
export BH61_RUN=bh61-YYYYMMDD-HHMMSS
export BH61_RF=$HOME/bh61-runs/$BH61_RUN
mkdir -p "$BH61_RF"
date -u +%Y-%m-%dT%H:%M:%S.%NZ | tee "$BH61_RF/start-utc.txt"
```

Record the hub serial, MAC address, firmware version, SDR serial number,
antenna, cables, attenuation, center frequency, sample rate, TX gain, and test
distance. Do not store a production credential in the RF evidence directory.

## 2. Verify the tools and radios

```sh
cd /path/to/verkada-verkracked-subghz-framework
ctest --test-dir build-native --output-on-failure
./build-native/bh61-radio devices --jsonl | tee "$BH61_RF/devices.jsonl"
hackrf_info | tee "$BH61_RF/hackrf-info.txt"
uhd_find_devices | tee "$BH61_RF/uhd-devices.txt"
rtl_test -t 2>&1 | tee "$BH61_RF/rtl-test.txt"
```

Stop if the selected SDR serial number is not present. An RTL-SDR can receive,
but it cannot transmit. A HackRF can transmit one stream. A B210 can schedule a
transmission and receive two coherent channels.

## 3. Make a passive baseline

Do this before transmission. Start the hub normally. Record at least one hub
state transition and one sensor event if a sensor is available.

```sh
./build-native/bh61-radio capture --backend hackrf --serial HACKRF_SERIAL \
  --output "$BH61_RF" --stem "$BH61_RUN-baseline" \
  --sample-count 40000000 --sample-rate 4000000 \
  --center-frequency 915350000 \
  --utc "$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)"

./build-native/bh61-radio decode \
  --input "$BH61_RF/$BH61_RUN-baseline.sigmf-meta" --jsonl \
  | tee "$BH61_RF/$BH61_RUN-baseline-decoded.jsonl"
```

The example center frequency is a laboratory placeholder in the documented US/Canada band.
Scan and replace it with the observed live channel. Preserve the original IQ
even if no frame decodes.

## 4. Validate a transmission without RF

```sh
./build-native/bh61-radio transmit \
  --frame-hex 1a41c800ff01010000000000000000000b00338b00000100000e3a \
  --output "$BH61_RF/dry-run" --stem "$BH61_RUN-dry" \
  --sample-rate 4000000 --center-frequency 915350000 --tx-gain 0 \
  --utc "$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)" --dry-run
```

Check the JSON record and SHA-256 manifest. The record must say
`dry_run:true`, `hardware_exact:false`, and `acknowledged:false`.

## 5. Make a normal antenna transmission

Use a shielded enclosure or the minimum measured power that gives a usable lab
link. Keep a second receiver on the channel so that you can prove what left the
antenna.

```sh
./build-native/bh61-radio transmit --backend hackrf \
  --serial HACKRF_SERIAL \
  --frame-hex 1a41c800ff01010000000000000000000b00338b00000100000e3a \
  --output "$BH61_RF/tx" --stem "$BH61_RUN-tx" \
  --sample-rate 4000000 --center-frequency 915350000 --tx-gain 0 \
  --utc "$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)" --enable-tx
```

The command proves that the host submitted the waveform to the SDR. It does not
prove that the BH61 received or accepted it. Prove acceptance with a correlated
hub response, sensor state, UART log, cloud event, or independent RF capture.

## 6. Prove isolation before a stateful exchange

Create a domain list that contains only the names used by your local emulator.
Then run the public preflight. It checks hub identity, the router drop rule,
local DNS, emulator health, a recent UART health oracle, the exact B210 serial,
USB 3 operation, and the FPGA digest.

```sh
tools/bh61_isolation_preflight.sh \
  --output "$BH61_RF/preflight.json" --hub-ip HUB_LAB_IP \
  --hub-mac HUB_LAB_MAC --dns-ip LAB_DNS_IP \
  --domains-file lab-domains.txt \
  --emulator-health-url http://LAB_EMULATOR_HOST:PORT/healthz \
  --router-host LAB_ROUTER --radio-host local \
  --b210-serial B210_SERIAL --fpga usrp_b210_fpga.bin \
  --fpga-sha256 FPGA_SHA256 --uart-oracle uart-health.txt
```

Stop if `overall` is not `pass`. The stateful commands also read this result
immediately before an exchange and fail closed if it is missing or negative.

## 7. Create and join a synthetic sensor

```sh
./build-native/bh61-radio sensor-create \
  --session "$BH61_RF/sensor-session" \
  --serial SYN-LAB-0001 --model door-contact

./build-native/bh61-radio sensor-join \
  --session "$BH61_RF/sensor-session" --output "$BH61_RF/join" \
  --preflight "$BH61_RF/preflight.json" --backend b210 \
  --serial B210_SERIAL --fpga usrp_b210_fpga.bin \
  --sample-rate 4000000 --center-frequency 915350000 \
  --rx-timeout-ms 2000 --tx-gain 0 --enable-tx
```

Keep `sensor-private.key` private. The join is accepted only when the normal
response and signature complete key establishment. A host-side transmission
alone is not a successful join.

## 8. Send an intended event and make a read-only request

```sh
./build-native/bh61-radio sensor-event \
  --session "$BH61_RF/sensor-session" --output "$BH61_RF/contact-opened" \
  --preflight "$BH61_RF/preflight.json" --backend b210 \
  --serial B210_SERIAL --fpga usrp_b210_fpga.bin \
  --sample-rate 4000000 --center-frequency 915350000 \
  --rx-timeout-ms 2000 --tx-gain 0 \
  --event contact-opened --event-id 1 --enable-tx

./build-native/bh61-radio sensor-rpc-read \
  --session "$BH61_RF/sensor-session" --output "$BH61_RF/image-state" \
  --preflight "$BH61_RF/preflight.json" --backend b210 \
  --serial B210_SERIAL --fpga usrp_b210_fpga.bin \
  --sample-rate 4000000 --center-frequency 915350000 \
  --rx-timeout-ms 2000 --tx-gain 0 --rpc image-state --enable-tx
```

Correlate the event identifier and runner evidence with the emulator journal.
See `STATEFUL-SENSORS.md` for the complete fixed allowlists.

## 9. Model sensor state without inventing RF bytes

The simulator creates correlated laboratory events. It does not invent unknown
sensor-event encodings.

```text
sensor zone-7
enroll
contact open
contact closed
motion active
motion clear
tamper open
battery 15
supervision
retransmit
```

```sh
./build-native/bh61-radio simulate --input scenario.txt \
  --output "$BH61_RF/$BH61_RUN-sensor.events.jsonl" \
  --stem "$BH61_RUN" --utc "$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)"
```

Use captured and verified frame templates before mapping these state events to
RF. Keep the state model and the RF encoder as separate evidence layers.

## 10. Make a coherent B210 measurement

Use equal cable lengths and a known antenna baseline. Measure a reference source
at a known position first. Enter the measured phase bias as the calibration
value.

```sh
./build-native/bh61-radio df-capture --backend uhd --serial B210_SERIAL \
  --output "$BH61_RF/df" --stem "$BH61_RUN-df" \
  --sample-count 4000000 --sample-rate 4000000 \
  --center-frequency 915350000 --antenna two-element-linear \
  --calibration-phase-rad 0.0 \
  --utc "$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)"
```

The result contains coherent phase and amplitude observations. It deliberately
sets `bearing_claimed:false`. Convert phase to a bearing only after you record
array geometry, cable calibration, ambiguity handling, and a validated model.

## 11. Export to Wireshark

```sh
./build-native/bh61-radio pcap --hex VALID_FRAME_HEX \
  --output "$BH61_RF/$BH61_RUN.pcap"
mkdir -p "$HOME/.local/lib/wireshark/plugins"
cp tools/wireshark/bh61.lua "$HOME/.local/lib/wireshark/plugins/bh61.lua"
tshark -r "$BH61_RF/$BH61_RUN.pcap" -V
```

The PCAP uses DLT_USER0. The Lua plugin displays the recovered PHR, VMAC
control, sequence, destination PAN, VCMP type, payload, and radio-FCS result.

## 12. Correlate RF with the cloud emulator

On the cloud-emulator host:

```sh
cd /path/to/verkada-verkracked-alarm-hub-local-framework
export ADMIN_TOKEN="$(tr -d '\r\n' < state/admin.token)"
./bh61-cloud-emulator lab-event --token "$ADMIN_TOKEN" \
  ingest "$BH61_RF/tx/$BH61_RUN-tx.events.jsonl"
./bh61-cloud-emulator lab-event --token "$ADMIN_TOKEN" list "$BH61_RUN-tx"
```

Open `http://ADMIN_HOST:18081/admin/v1/lab/dashboard`. Enter the administration
token in the dashboard. Filter with the same correlation ID. The correlation
endpoint is `/admin/v1/lab/correlations/CORRELATION_ID`.

## 13. Run deterministic cloud faults

Create a reviewed profile:

```json
{
  "enabled": true,
  "path_prefix": "/device_instance/",
  "status": 503,
  "every_nth": 3,
  "delay_ms": 250,
  "body": "bh61-lab-injected-fault"
}
```

```sh
./bh61-cloud-emulator fault --token "$ADMIN_TOKEN" \
  set checkin-503 fault.json
./bh61-cloud-emulator fault --token "$ADMIN_TOKEN" list
```

Disable a profile by setting `enabled` to `false`. Each hit is persisted. The
request journal classifies an applied response as `lab-fault:PROFILE_NAME`.

## 14. Exercise update behavior

Publish only an owned test artifact. A published artifact is immutable and
addressed by SHA-256.

```sh
./bh61-cloud-emulator artifact --token "$ADMIN_TOKEN" \
  publish sensor TEST_VERSION /absolute/path/test.gbl test.gbl
./bh61-cloud-emulator update --token "$ADMIN_TOKEN" \
  expected-set sensor TEST_VERSION
./bh61-cloud-emulator update --token "$ADMIN_TOKEN" timeline DEVICE_ID
```

Use the request journal and timeline to distinguish URL issuance, range or full
download, status report, and boot confirmation. Point the expected version to a
previously published version to test rollback behavior. Do not call a download
an installation unless the device reports and confirms the new version.

## 15. Run fleet and regression checks

The fleet file must contain an explicit scenario ID, owned device identities,
and valid `bh61.lab.event/v1` objects. The emulator permits at most 1,000 devices
and 100,000 events in one scenario.

```sh
./bh61-cloud-emulator fleet --token "$ADMIN_TOKEN" run fleet.json
./bh61-cloud-emulator snapshot --token "$ADMIN_TOKEN" \
  create before FIRMWARE_VERSION
# Run the controlled action.
./bh61-cloud-emulator snapshot --token "$ADMIN_TOKEN" \
  create after FIRMWARE_VERSION
./bh61-cloud-emulator snapshot --token "$ADMIN_TOKEN" compare before after
```

The snapshot digest covers firmware label, device count, event-type counts, and
request-class counts. Preserve the input scenario with the output.

## 16. Export a forensic bundle

```sh
./bh61-cloud-emulator forensics --token "$ADMIN_TOKEN" \
  export "$BH61_RF/$BH61_RUN-cloud-forensics.json"
chmod 600 "$BH61_RF/$BH61_RUN-cloud-forensics.json"
sha256sum "$BH61_RF/$BH61_RUN-cloud-forensics.json" \
  > "$BH61_RF/$BH61_RUN-cloud-forensics.json.sha256"
```

The bundle contains the complete emulator state and request journal. It also
contains a content digest over those two sections. It can contain device tokens
and command output. Keep it confidential.

## 17. Close the run

```sh
date -u +%Y-%m-%dT%H:%M:%S.%NZ | tee "$BH61_RF/end-utc.txt"
find "$BH61_RF" -type f -print0 | sort -z | xargs -0 sha256sum \
  > "$BH61_RF/all-files.sha256"
```

Record each result as one of: confirmed, rejected, not observed, blocked, or
not tested. Keep host-side transmission, over-air observation, device receipt,
device acceptance, and cloud effect as separate claims.
