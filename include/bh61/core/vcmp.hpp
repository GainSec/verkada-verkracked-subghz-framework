#pragma once

#include "bh61/core/frame.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace bh61::core {

struct VcmpPlaintext {
  std::vector<std::uint8_t> bytes;
};

struct VcmpEncrypted {
  std::array<std::uint8_t, 16> iv{};
  std::uint8_t plaintext_length{};
  std::vector<std::uint8_t> ciphertext;
};

struct VcmpFrame {
  std::uint8_t type{};
  std::uint8_t flags{};
  std::uint16_t received_crc{};
  std::optional<std::uint16_t> computed_crc;
  std::variant<VcmpPlaintext, VcmpEncrypted> body;
};

using VcmpResult = std::variant<VcmpFrame, ParseError>;

auto parse_vcmp(std::span<const std::uint8_t> bytes) -> VcmpResult;
auto encode_vcmp(const VcmpFrame& frame) -> std::vector<std::uint8_t>;

}  // namespace bh61::core
