#include "bh61/dsp/dsss.hpp"

#include <array>
#include <limits>
#include <stdexcept>

namespace bh61::dsp {
namespace {

constexpr std::array<std::uint32_t, 16> dictionary{
    0xc8dd7892, 0x2c8dd789, 0x92c8dd78, 0x892c8dd7,
    0x7892c8dd, 0xd7892c8d, 0xdd7892c8, 0x8dd7892c,
    0x9d882dc7, 0x79d882dc, 0xc79d882d, 0xdc79d882,
    0x2dc79d88, 0x82dc79d8, 0x882dc79d, 0xd882dc79};

}  // namespace

auto dsss_word(std::uint8_t symbol) -> std::uint32_t {
  if (symbol >= dictionary.size()) {
    throw std::invalid_argument("DSSS symbol must be in range 0..15");
  }
  return dictionary[symbol];
}

auto dsss_chips(std::uint8_t symbol) -> ChipSequence {
  const auto word = dsss_word(symbol);
  ChipSequence chips{};
  for (std::size_t index = 0; index < chips.size(); ++index) {
    chips[index] = static_cast<std::uint8_t>((word >> index) & 1U);
  }
  return chips;
}

auto bytes_to_symbols(std::span<const std::uint8_t> bytes)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> symbols;
  symbols.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    symbols.push_back(static_cast<std::uint8_t>(byte & 0x0fU));
    symbols.push_back(static_cast<std::uint8_t>(byte >> 4U));
  }
  return symbols;
}

auto decode_hard(std::span<const std::uint8_t, 32> chips)
    -> HardSymbolDecision {
  HardSymbolDecision result{};
  result.distance = std::numeric_limits<std::size_t>::max();
  result.runner_up_distance = std::numeric_limits<std::size_t>::max();
  for (std::uint8_t symbol = 0; symbol < dictionary.size(); ++symbol) {
    const auto candidate = dsss_chips(symbol);
    std::size_t distance = 0;
    for (std::size_t index = 0; index < candidate.size(); ++index) {
      if (chips[index] > 1U) {
        throw std::invalid_argument("hard DSSS chips must be zero or one");
      }
      distance += chips[index] == candidate[index] ? 0U : 1U;
    }
    if (distance < result.distance) {
      result.runner_up_distance = result.distance;
      result.distance = distance;
      result.symbol = symbol;
      result.ambiguous = false;
    } else if (distance == result.distance) {
      result.runner_up_distance = distance;
      result.ambiguous = true;
    } else if (distance < result.runner_up_distance) {
      result.runner_up_distance = distance;
    }
  }
  return result;
}

}  // namespace bh61::dsp
