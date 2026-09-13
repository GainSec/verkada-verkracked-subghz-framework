#include "bh61/dsp/profile.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <vector>

BH61_TEST("PHY profile exposes every recovered fixed parameter") {
  BH61_REQUIRE(bh61::dsp::phy::bit_rate == 80'000U);
  BH61_REQUIRE(bh61::dsp::phy::symbol_rate == 20'000U);
  BH61_REQUIRE(bh61::dsp::phy::chip_rate == 640'000U);
  BH61_REQUIRE(bh61::dsp::phy::preamble_symbols == 10U);
  BH61_REQUIRE(bh61::dsp::phy::sync_symbols ==
               (std::array<std::uint8_t, 3>{0x00, 0x07, 0x0a}));
  BH61_REQUIRE(bh61::dsp::phy::custom_oqpsk_taps ==
               (std::array<std::uint8_t, 8>{1, 1, 16, 48,
                                             80, 112, 127, 127}));
  BH61_REQUIRE(!bh61::dsp::phy::custom_tap_application_is_iq_verified);
}

BH61_TEST("FCC channel formula is exact and bounded") {
  BH61_REQUIRE(bh61::dsp::phy::fcc_frequency_hz(0) == 915'000'000U);
  BH61_REQUIRE(bh61::dsp::phy::fcc_frequency_hz(10) == 915'350'000U);
  BH61_REQUIRE(bh61::dsp::phy::fcc_frequency_hz(20) == 915'700'000U);
}

BH61_TEST("burst symbol stream has exact training and nibble order") {
  const std::array<std::uint8_t, 2> frame{0x1a, 0x41};
  const auto symbols = bh61::dsp::phy::burst_symbols(frame);
  BH61_REQUIRE(symbols.size() == 17U);
  for (std::size_t index = 0; index < 11U; ++index) {
    BH61_REQUIRE(symbols[index] == 0U);
  }
  BH61_REQUIRE(symbols[11] == 0x07);
  BH61_REQUIRE(symbols[12] == 0x0a);
  BH61_REQUIRE(symbols[13] == 0x0a);
  BH61_REQUIRE(symbols[14] == 0x01);
  BH61_REQUIRE(symbols[15] == 0x01);
  BH61_REQUIRE(symbols[16] == 0x04);
}
