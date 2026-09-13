#include "bh61/dsp/demodulator.hpp"
#include "bh61/dsp/modulator.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 27> exact_type11_frame{
    0x1a, 0x41, 0xc8, 0x00, 0xff, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00,
    0x33, 0x8b, 0x00, 0x00, 0x01, 0x00, 0x00, 0x70, 0x5c};

}  // namespace

BH61_TEST("clean analytical IQ round-trips through acquisition and FCS") {
  const auto waveform = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  const auto result = bh61::dsp::decode_first_burst(
      waveform.samples, waveform.sample_rate, {});
  BH61_REQUIRE(std::holds_alternative<bh61::dsp::DecodedBurst>(result));
  const auto& decoded = std::get<bh61::dsp::DecodedBurst>(result);
  BH61_REQUIRE(decoded.frame_bytes ==
               std::vector<std::uint8_t>(exact_type11_frame.begin(),
                                         exact_type11_frame.end()));
  BH61_REQUIRE(decoded.acquisition.start_sample == 0U);
  BH61_REQUIRE(decoded.acquisition.score > 0.99);
  BH61_REQUIRE(decoded.radio_frame.received_fcs == 0x705c);
}

BH61_TEST("decoder requires a valid PHR and radio FCS after correlation") {
  auto corrupt = exact_type11_frame;
  corrupt[20] ^= 0x01;
  const auto waveform = bh61::dsp::modulate_oqpsk(
      corrupt, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  const auto result = bh61::dsp::decode_first_burst(
      waveform.samples, waveform.sample_rate, {});
  BH61_REQUIRE(std::holds_alternative<bh61::dsp::DecodeError>(result));
  BH61_REQUIRE(std::get<bh61::dsp::DecodeError>(result).code ==
               bh61::dsp::DecodeErrorCode::InvalidFrame);
}
