#pragma once

#include "bh61/radio/device.hpp"
#include "bh61/radio/device_factory.hpp"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace bh61::app {

enum class Command {
  Help,
  Version,
  Devices,
  Inspect,
  Build,
  SecureFrame,
  JoinFrame,
  ScanCompleteFrame,
  Waveform,
  Decode,
  Capture,
  Scan,
  Fixtures,
  Transmit,
  Simulate,
  Pcap,
  Df,
  DfCapture,
  Coverage,
  SensorCreate,
  SensorJoin,
  SensorEvent,
  SensorRpcRead,
  SensorStatus,
  SensorRetire,
};

struct Arguments {
  Command command{Command::Help};
  bool jsonl{};
  bool enable_tx{};
  bool dry_run{};
  bool request_uplink_mac_ack{};
  std::optional<std::string> hex;
  std::optional<std::string> psdu_hex;
  std::optional<std::string> frame_hex;
  std::optional<std::string> source_eui;
  std::optional<std::string> payload_hex;
  std::optional<std::string> key_hex;
  std::optional<std::string> iv_hex;
  std::optional<std::string> serial_hex;
  std::optional<std::string> input;
  std::optional<std::string> input_b;
  std::optional<std::string> output;
  std::optional<std::string> backend;
  std::optional<std::string> serial;
  std::optional<std::string> fpga;
  std::optional<std::string> location;
  std::optional<std::string> antenna;
  std::optional<std::string> session;
  std::optional<std::string> preflight;
  std::optional<std::string> model;
  std::optional<std::string> event;
  std::optional<std::string> rpc;
  std::string stem{"capture"};
  std::string utc{"1970-01-01T00:00:00.000000Z"};
  std::string waveform_model{"efr32-custom-oqpsk-mode2"};
  std::uint32_t sample_rate{4'000'000U};
  bool sample_rate_explicit{};
  std::uint64_t center_frequency_hz{915'350'000U};
  std::size_t sample_count{};
  std::size_t repeat_count{1U};
  std::uint64_t interval_ms{};
  std::uint64_t requested_time_ns{};
  double calibration_phase_rad{};
  double carrier_offset_hz{};
  double amplitude{0.5};
  double gain_db{};
  double score{};
  std::size_t packet_count{};
  std::size_t valid_count{};
  std::uint32_t tx_gain_db{};
  std::optional<std::uint32_t> destination;
  std::optional<std::uint32_t> pan;
  std::optional<std::uint32_t> sequence;
  std::optional<std::uint32_t> type;
  std::optional<std::uint32_t> flags;
  std::optional<std::uint32_t> counter;
  std::optional<std::uint32_t> channel;
  std::optional<std::uint32_t> regulatory_code;
  std::optional<std::uint32_t> scan_sequence;
  std::optional<std::uint32_t> event_id;
  std::optional<std::uint32_t> config_id;
  std::optional<std::uint32_t> stat_group;
  std::optional<std::uint32_t> index;
  std::uint64_t rx_timeout_ms{2'000U};
};

struct ArgumentError {
  std::string code;
  std::string message;
};

using ArgumentResult = std::variant<Arguments, ArgumentError>;

struct CliEnvironment {
  radio::Device* device{};
  radio::DeviceFactory* factory{};
};

auto parse_arguments(std::span<const std::string_view> arguments)
    -> ArgumentResult;
auto run_cli(std::span<const std::string_view> arguments, std::ostream& output,
             std::ostream& error, CliEnvironment environment = {}) -> int;

}  // namespace bh61::app
