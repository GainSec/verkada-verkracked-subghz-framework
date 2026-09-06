#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace bh61::core {

constexpr auto load_be16(std::span<const std::uint8_t, 2> bytes)
    -> std::uint16_t {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
}

constexpr auto load_le16(std::span<const std::uint8_t, 2> bytes)
    -> std::uint16_t {
  return static_cast<std::uint16_t>(
      bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8U));
}

constexpr auto store_be16(std::uint16_t value)
    -> std::array<std::uint8_t, 2> {
  return {static_cast<std::uint8_t>(value >> 8U),
          static_cast<std::uint8_t>(value)};
}

constexpr auto store_le16(std::uint16_t value)
    -> std::array<std::uint8_t, 2> {
  return {static_cast<std::uint8_t>(value),
          static_cast<std::uint8_t>(value >> 8U)};
}

constexpr auto load_be32(std::span<const std::uint8_t, 4> bytes)
    -> std::uint32_t {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

constexpr auto store_be32(std::uint32_t value)
    -> std::array<std::uint8_t, 4> {
  return {static_cast<std::uint8_t>(value >> 24U),
          static_cast<std::uint8_t>(value >> 16U),
          static_cast<std::uint8_t>(value >> 8U),
          static_cast<std::uint8_t>(value)};
}

}  // namespace bh61::core
