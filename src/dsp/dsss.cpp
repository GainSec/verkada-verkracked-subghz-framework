#include "bh61/dsp/dsss.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace bh61::dsp {
namespace {

constexpr std::array<std::uint32_t, 16> dictionary{
    0xc8dd7892, 0x8dd7892c, 0xdd7892c8, 0xd7892c8d,
    0x7892c8dd, 0x892c8dd7, 0x92c8dd78, 0x2c8dd789,
    0x6277d238, 0x277d2386, 0x77d23862, 0x7d238627,
    0xd2386277, 0x2386277d, 0x386277d2, 0x86277d23};

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

auto decode_soft(std::span<const double, 32> chip_confidence)
    -> SoftSymbolDecision {
  SoftSymbolDecision result{};
  result.score = -std::numeric_limits<double>::infinity();
  result.runner_up_score = -std::numeric_limits<double>::infinity();
  for (std::uint8_t symbol = 0; symbol < dictionary.size(); ++symbol) {
    const auto candidate = dsss_chips(symbol);
    double score = 0.0;
    std::size_t hard_distance = 0U;
    for (std::size_t index = 0U; index < candidate.size(); ++index) {
      if (!std::isfinite(chip_confidence[index])) {
        throw std::invalid_argument("soft DSSS chips must be finite");
      }
      score += candidate[index] == 0U ? chip_confidence[index]
                                      : -chip_confidence[index];
      const auto hard_chip = chip_confidence[index] >= 0.0 ? 0U : 1U;
      hard_distance += hard_chip == candidate[index] ? 0U : 1U;
    }
    if (score > result.score) {
      result.runner_up_score = result.score;
      result.score = score;
      result.symbol = symbol;
      result.hard_distance = hard_distance;
      result.ambiguous = false;
    } else if (score == result.score) {
      result.runner_up_score = score;
      result.ambiguous = true;
    } else if (score > result.runner_up_score) {
      result.runner_up_score = score;
    }
  }
  return result;
}

}  // namespace bh61::dsp
