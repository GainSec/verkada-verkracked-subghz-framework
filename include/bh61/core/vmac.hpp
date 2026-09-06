#pragma once

#include "bh61/core/frame.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace bh61::core {

struct VmacFrame {
  std::uint16_t frame_control{};
  std::uint8_t sequence{};
  std::uint16_t destination_pan{};
  std::variant<std::uint16_t, std::array<std::uint8_t, 8>> destination;
  std::array<std::uint8_t, 8> source_eui{};
  std::vector<std::uint8_t> payload;
};

using VmacResult = std::variant<VmacFrame, ParseError>;

auto parse_vmac(std::span<const std::uint8_t> psdu) -> VmacResult;
auto encode_vmac(const VmacFrame& frame) -> std::vector<std::uint8_t>;
auto destination_is_accepted(
    const VmacFrame& frame,
    const std::array<std::uint8_t, 8>& local_eui) noexcept -> bool;

}  // namespace bh61::core
