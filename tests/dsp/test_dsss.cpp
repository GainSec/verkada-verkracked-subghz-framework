#include "bh61/dsp/dsss.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr std::array<std::uint32_t, 16> expected_words{
    0xc8dd7892, 0x2c8dd789, 0x92c8dd78, 0x892c8dd7,
    0x7892c8dd, 0xd7892c8d, 0xdd7892c8, 0x8dd7892c,
    0x9d882dc7, 0x79d882dc, 0xc79d882d, 0xdc79d882,
    0x2dc79d88, 0x82dc79d8, 0x882dc79d, 0xd882dc79};

const std::array<std::string, 16> expected_chips{
    "01001001000111101011101100010011",
    "10010001111010111011000100110100",
    "00011110101110110001001101001001",
    "11101011101100010011010010010001",
    "10111011000100110100100100011110",
    "10110001001101001001000111101011",
    "00010011010010010001111010111011",
    "00110100100100011110101110110001",
    "11100011101101000001000110111001",
    "00111011010000010001101110011110",
    "10110100000100011011100111100011",
    "01000001000110111001111000111011",
    "00010001101110011110001110110100",
    "00011011100111100011101101000001",
    "10111001111000111011010000010001",
    "10011110001110110100000100011011"};

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
