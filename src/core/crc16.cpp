#include "bh61/core/crc16.hpp"

namespace bh61::core {
namespace {

constexpr std::uint16_t polynomial = 0x1021;

constexpr auto reverse_bits(std::uint8_t value) -> std::uint8_t {
  value = static_cast<std::uint8_t>(((value & 0x55U) << 1U) |
                                    ((value & 0xaaU) >> 1U));
  value = static_cast<std::uint8_t>(((value & 0x33U) << 2U) |
                                    ((value & 0xccU) >> 2U));
  return static_cast<std::uint8_t>((value << 4U) | (value >> 4U));
}

auto ccitt_msb(std::span<const std::uint8_t> bytes, bool reflect_input)
    -> std::uint16_t {
  std::uint16_t remainder = 0;
  for (const auto raw_byte : bytes) {
    const auto byte = reflect_input ? reverse_bits(raw_byte) : raw_byte;
    remainder ^= static_cast<std::uint16_t>(byte) << 8U;
    for (int bit = 0; bit < 8; ++bit) {
      remainder = (remainder & 0x8000U) != 0
                      ? static_cast<std::uint16_t>((remainder << 1U) ^ polynomial)
                      : static_cast<std::uint16_t>(remainder << 1U);
    }
  }
  return remainder;
}

}  // namespace

auto vcmp_crc16(std::span<const std::uint8_t> bytes) noexcept -> std::uint16_t {
  return ccitt_msb(bytes, false);
}

auto radio_fcs16(std::span<const std::uint8_t> bytes) noexcept -> std::uint16_t {
  return ccitt_msb(bytes, true);
}

}  // namespace bh61::core
