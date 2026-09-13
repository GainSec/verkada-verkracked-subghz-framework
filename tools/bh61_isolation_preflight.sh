#!/bin/sh
set -u

output=
hub_ip=
hub_mac=
dns_ip=
domains_file=
emulator_health_url=
router_host=
radio_host=local
b210_serial=
fpga_path=
fpga_sha256=
uart_oracle=

usage() {
  printf '%s\n' \
    'usage: bh61_isolation_preflight.sh --output FILE --hub-ip IP --hub-mac MAC' \
    '  --dns-ip IP --domains-file FILE --emulator-health-url URL' \
    '  --router-host HOST --b210-serial SERIAL --fpga FILE' \
    '  --fpga-sha256 SHA256 --uart-oracle FILE [--radio-host HOST|local]'
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --output) output=${2-}; shift 2 ;;
    --hub-ip) hub_ip=${2-}; shift 2 ;;
    --hub-mac) hub_mac=${2-}; shift 2 ;;
    --dns-ip) dns_ip=${2-}; shift 2 ;;
    --domains-file) domains_file=${2-}; shift 2 ;;
    --emulator-health-url) emulator_health_url=${2-}; shift 2 ;;
    --router-host) router_host=${2-}; shift 2 ;;
    --radio-host) radio_host=${2-}; shift 2 ;;
    --b210-serial) b210_serial=${2-}; shift 2 ;;
    --fpga) fpga_path=${2-}; shift 2 ;;
    --fpga-sha256) fpga_sha256=${2-}; shift 2 ;;
    --uart-oracle) uart_oracle=${2-}; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

for required in "$output" "$hub_ip" "$hub_mac" "$dns_ip" "$domains_file" \
  "$emulator_health_url" "$router_host" "$b210_serial" "$fpga_path" \
  "$fpga_sha256" "$uart_oracle"; do
  [ -n "$required" ] || { usage >&2; exit 2; }
done
case "$hub_ip$dns_ip" in *[!0-9.:]*) printf '%s\n' 'invalid IP value' >&2; exit 2;; esac
case "$hub_mac" in
  [0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]) ;;
  *) printf '%s\n' 'invalid hub MAC' >&2; exit 2 ;;
esac
case "$fpga_sha256" in *[!0-9a-fA-F]*) printf '%s\n' 'invalid FPGA SHA-256' >&2; exit 2;; esac
[ "${#fpga_sha256}" -eq 64 ] || { printf '%s\n' 'invalid FPGA SHA-256' >&2; exit 2; }
case "$output$domains_file$router_host$radio_host$b210_serial$fpga_path$uart_oracle" in
  *[!A-Za-z0-9_./:@+-]*) printf '%s\n' 'argument contains unsupported characters' >&2; exit 2 ;;
esac
case "$emulator_health_url" in http://*|https://*) ;; *) printf '%s\n' 'health URL must use HTTP or HTTPS' >&2; exit 2;; esac
case "$emulator_health_url" in
  *[!A-Za-z0-9_./:@%+=-]*) printf '%s\n' 'health URL contains unsupported characters' >&2; exit 2 ;;
esac
[ -f "$domains_file" ] || { printf '%s\n' 'domains file is missing' >&2; exit 2; }
[ ! -e "$output" ] && [ ! -e "$output.raw" ] || {
  printf '%s\n' 'preflight output already exists' >&2; exit 2;
}

raw_directory="$output.raw"
mkdir -m 700 "$raw_directory" || exit 2
run_radio() {
  if [ "$radio_host" = local ]; then /bin/sh -c "$1"; else ssh "$radio_host" "$1"; fi
}
probe() {
  gate=$1
  case "$gate" in
    hub_identity) ssh "$router_host" "ip neigh show to $hub_ip" ;;
    isolation) ssh "$router_host" "nft list ruleset" ;;
    dns)
      while IFS= read -r name; do
        case "$name" in ''|'#'*) continue;; *[!A-Za-z0-9.-]*) return 2;; esac
        answer=$(run_radio "dig +time=2 +tries=1 +short @$dns_ip $name A") || return 1
        printf '%s=%s\n' "$name" "$answer"
      done <"$domains_file" ;;
    emulator_health) run_radio "curl --silent --show-error --fail '$emulator_health_url'" ;;
    uart_oracle)
      run_radio "test -f '$uart_oracle' && age=\$((\$(date +%s)-\$(stat -c %Y '$uart_oracle'))) && test \"\$age\" -le 30 && grep -qx healthy '$uart_oracle' && printf 'healthy age_seconds=%s\\n' \"\$age\"" ;;
    b210_identity) run_radio "uhd_find_devices --args 'serial=$b210_serial'" ;;
    b210_usb3) run_radio "uhd_usrp_probe --args 'serial=$b210_serial,fpga=$fpga_path' 2>&1" ;;
    fpga_hash) run_radio "sha256sum '$fpga_path'" ;;
    *) return 2 ;;
  esac
}
valid() {
  gate=$1 file=$2
  case "$gate" in
    hub_identity) grep -Fq "$hub_ip" "$file" && grep -Fiq "$hub_mac" "$file" ;;
    isolation) grep -Fq "$hub_ip" "$file" && grep -Fq 'drop' "$file" ;;
    dns) [ -s "$file" ] && ! grep -Eq '=$' "$file" ;;
    emulator_health) grep -Eq '(^ok$)|("status"[[:space:]]*:[[:space:]]*"ok")' "$file" ;;
    uart_oracle) grep -Eq '^healthy age_seconds=([0-9]|[12][0-9]|30)$' "$file" ;;
    b210_identity) grep -Fq "$b210_serial" "$file" && grep -Eiq 'B210|B2[01]0' "$file" ;;
    b210_usb3) grep -Eiq 'USB[[:space:]]*3|SuperSpeed' "$file" && ! grep -Eiq 'USB[[:space:]]*2' "$file" ;;
    fpga_hash) grep -Eiq "^$fpga_sha256[[:space:]]" "$file" ;;
  esac
}

gates='hub_identity isolation dns emulator_health uart_oracle b210_identity b210_usb3 fpga_hash'
overall=pass
for gate in $gates; do
  raw="$raw_directory/$gate.txt"
  if probe "$gate" >"$raw" 2>&1 && valid "$gate" "$raw"; then
    eval "status_$gate=pass"
  else
    eval "status_$gate=fail"
    overall=fail
  fi
done

timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)
temporary="$output.tmp.$$"
umask 077
{
  printf '{"schema":"bh61.isolation-preflight/v1","timestamp_utc":"%s","overall":"%s","gates":{' "$timestamp" "$overall"
  separator=
  for gate in $gates; do
    eval "gate_status=\$status_$gate"
    printf '%s"%s":{"status":"%s","raw":"%s/%s.txt"}' "$separator" "$gate" "$gate_status" "$raw_directory" "$gate"
    separator=,
  done
  printf '}}\n'
} >"$temporary" || exit 2
mv "$temporary" "$output" || exit 2
[ "$overall" = pass ]
