#include "bh61/dsp/dsss.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr std::array<std::uint32_t, 16> expected_words{
    0xc8dd7892, 0x8dd7892c, 0xdd7892c8, 0xd7892c8d,
    0x7892c8dd, 0x892c8dd7, 0x92c8dd78, 0x2c8dd789,
    0x6277d238, 0x277d2386, 0x77d23862, 0x7d238627,
    0xd2386277, 0x2386277d, 0x386277d2, 0x86277d23};

const std::array<std::string, 16> expected_chips{
    "01001001000111101011101100010011",
    "00110100100100011110101110110001",
    "00010011010010010001111010111011",
    "10110001001101001001000111101011",
    "10111011000100110100100100011110",
    "11101011101100010011010010010001",
    "00011110101110110001001101001001",
    "10010001111010111011000100110100",
    "00011100010010111110111001000110",
    "01100001110001001011111011100100",
    "01000110000111000100101111101110",
    "11100100011000011100010010111110",
    "11101110010001100001110001001011",
    "10111110111001000110000111000100",
    "01001011111011100100011000011100",
    "11000100101111101110010001100001"};

auto chips_to_string(const bh61::dsp::ChipSequence& chips) -> std::string {
  std::string result;
  result.reserve(chips.size());
  for (const auto chip : chips) {
    result.push_back(chip == 0 ? '0' : '1');
  }
  return result;
}

}  // namespace

BH61_TEST("DSSS dictionary matches all recovered words and serialized chips") {
  for (std::uint8_t symbol = 0; symbol < 16U; ++symbol) {
    BH61_REQUIRE(bh61::dsp::dsss_word(symbol) == expected_words[symbol]);
    BH61_REQUIRE(chips_to_string(bh61::dsp::dsss_chips(symbol)) ==
                 expected_chips[symbol]);
  }
}

BH61_TEST("frame bytes emit low nibble before high nibble") {
  const auto symbols =
      bh61::dsp::bytes_to_symbols(std::array<std::uint8_t, 2>{0x36, 0x1a});
  BH61_REQUIRE(symbols ==
               (std::vector<std::uint8_t>{0x06, 0x03, 0x0a, 0x01}));
}

BH61_TEST("hard decoder identifies every code with each individual chip flipped") {
  for (std::uint8_t symbol = 0; symbol < 16U; ++symbol) {
    const auto exact = bh61::dsp::decode_hard(bh61::dsp::dsss_chips(symbol));
    BH61_REQUIRE(!exact.ambiguous);
    BH61_REQUIRE(exact.symbol == symbol);
    BH61_REQUIRE(exact.distance == 0);
    for (std::size_t index = 0; index < 32U; ++index) {
      auto damaged = bh61::dsp::dsss_chips(symbol);
      damaged[index] ^= 1U;
      const auto decision = bh61::dsp::decode_hard(damaged);
      BH61_REQUIRE(!decision.ambiguous);
      BH61_REQUIRE(decision.symbol == symbol);
      BH61_REQUIRE(decision.distance == 1);
    }
  }
}

BH61_TEST("hard decoder reports an equal-distance tie as ambiguous") {
  const auto first = bh61::dsp::dsss_chips(0);
  const auto second = bh61::dsp::dsss_chips(1);
  auto midpoint = first;
  std::size_t changed = 0;
  std::size_t differences = 0;
  for (std::size_t index = 0; index < midpoint.size(); ++index) {
    if (first[index] != second[index]) {
      if ((differences % 2U) == 0U) {
        midpoint[index] = second[index];
        ++changed;
      }
      ++differences;
    }
  }
  BH61_REQUIRE(changed * 2U == differences);
  const auto decision = bh61::dsp::decode_hard(midpoint);
  BH61_REQUIRE(decision.ambiguous);
}
