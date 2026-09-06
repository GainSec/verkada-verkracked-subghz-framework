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
  Waveform,
  Decode,
  Capture,
  Scan,
  Fixtures,
};

struct Arguments {
  Command command{Command::Help};
  bool jsonl{};
  std::optional<std::string> hex;
  std::optional<std::string> psdu_hex;
  std::optional<std::string> frame_hex;
  std::optional<std::string> input;
  std::optional<std::string> output;
  std::optional<std::string> backend;
  std::optional<std::string> serial;
  std::string stem{"capture"};
  std::string utc{"1970-01-01T00:00:00.000000Z"};
  std::uint32_t sample_rate{4'000'000U};
  bool sample_rate_explicit{};
  std::uint64_t center_frequency_hz{915'350'000U};
  std::size_t sample_count{};
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
