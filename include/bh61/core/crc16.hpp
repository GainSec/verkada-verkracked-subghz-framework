#pragma once

#include <cstdint>
#include <span>

namespace bh61::core {

auto vcmp_crc16(std::span<const std::uint8_t> bytes) noexcept -> std::uint16_t;
auto radio_fcs16(std::span<const std::uint8_t> bytes) noexcept -> std::uint16_t;

}  // namespace bh61::core
