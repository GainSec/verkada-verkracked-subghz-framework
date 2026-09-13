#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bh61::core {

enum class ParseErrorCode {
  Truncated,
  InvalidLength,
  LengthMismatch,
  BadFcs,
  UnsupportedAddressing,
  InvalidDestination,
  BadChecksum,
  InvalidEncoding,
  InvalidField,
  UnsupportedVersion,
};

struct ParseError {
  ParseErrorCode code;
  std::string layer;
  std::size_t offset;
  std::string reason;
};

struct RadioFrame {
  std::uint8_t phr{};
  std::vector<std::uint8_t> psdu;
  std::uint16_t received_fcs{};
  std::uint16_t computed_fcs{};
};

using RadioFrameResult = std::variant<RadioFrame, ParseError>;

auto parse_radio_frame(std::span<const std::uint8_t> bytes)
    -> RadioFrameResult;
auto encode_radio_frame(const RadioFrame& frame) -> std::vector<std::uint8_t>;
auto encode_mac_ack_frame(std::uint8_t sequence) -> std::vector<std::uint8_t>;

}  // namespace bh61::core
