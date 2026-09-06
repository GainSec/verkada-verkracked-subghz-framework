#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bh61::dsp {

using ChipSequence = std::array<std::uint8_t, 32>;

struct HardSymbolDecision {
  std::uint8_t symbol{};
  std::size_t distance{};
  std::size_t runner_up_distance{};
  bool ambiguous{};
};

auto dsss_word(std::uint8_t symbol) -> std::uint32_t;
auto dsss_chips(std::uint8_t symbol) -> ChipSequence;
auto bytes_to_symbols(std::span<const std::uint8_t> bytes)
    -> std::vector<std::uint8_t>;
auto decode_hard(std::span<const std::uint8_t, 32> chips)
    -> HardSymbolDecision;

}  // namespace bh61::dsp
