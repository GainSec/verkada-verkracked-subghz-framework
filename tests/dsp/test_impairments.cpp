#include "bh61/dsp/demodulator.hpp"
#include "bh61/dsp/modulator.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <span>
#include <variant>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 27> exact_type11_frame{
    0x1a, 0x41, 0xc8, 0x00, 0xff, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00,
    0x33, 0x8b, 0x00, 0x00, 0x01, 0x00, 0x00, 0x70, 0x5c};

auto impaired(std::vector<std::complex<float>> samples,
              std::uint32_t sample_rate, std::size_t leading_zeros,
              double phase, double carrier_hz, bool conjugate)
    -> std::vector<std::complex<float>> {
  std::vector<std::complex<float>> output(leading_zeros, {0.0F, 0.0F});
  output.reserve(leading_zeros + samples.size());
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const auto angle = phase +
        2.0 * std::numbers::pi * carrier_hz *
            static_cast<double>(index + leading_zeros) /
            static_cast<double>(sample_rate);
    const std::complex<float> rotation{
        static_cast<float>(std::cos(angle)),
        static_cast<float>(std::sin(angle))};
    auto value = samples[index] * rotation * 0.63F;
    if (conjugate) {
      value = std::conj(value);
    }
    output.push_back(value);
  }
  return output;
}

auto resample_clock(std::span<const std::complex<float>> input, double ratio)
    -> std::vector<std::complex<float>> {
  const auto output_size = static_cast<std::size_t>(
      std::floor(static_cast<double>(input.size()) * ratio));
  std::vector<std::complex<float>> output;
  output.reserve(output_size);
  for (std::size_t index = 0; index < output_size; ++index) {
    const auto source = static_cast<double>(index) / ratio;
    const auto lower = static_cast<std::size_t>(std::floor(source));
    const auto fraction = static_cast<float>(source -
                                             static_cast<double>(lower));
    const auto upper = std::min(lower + 1U, input.size() - 1U);
    output.push_back(input[lower] * (1.0F - fraction) +
                     input[upper] * fraction);
  }
  return output;
}

auto add_deterministic_noise(std::span<const std::complex<float>> input,
                             float amplitude)
    -> std::vector<std::complex<float>> {
  std::vector<std::complex<float>> output(input.begin(), input.end());
  std::uint32_t state = 0x6b613631U;
  for (auto& sample : output) {
    state = state * 1'664'525U + 1'013'904'223U;
    const auto real = static_cast<float>((state >> 8U) & 0xffffU) /
                          32'767.5F -
                      1.0F;
    state = state * 1'664'525U + 1'013'904'223U;
    const auto imag = static_cast<float>((state >> 8U) & 0xffffU) /
                          32'767.5F -
                      1.0F;
    sample += std::complex<float>{real * amplitude, imag * amplitude};
  }
  return output;
}

auto require_exact_decode(std::span<const std::complex<float>> samples,
                          std::uint32_t sample_rate) -> void {
  const auto result = bh61::dsp::decode_first_burst(samples, sample_rate, {});
  BH61_REQUIRE(std::holds_alternative<bh61::dsp::DecodedBurst>(result));
  BH61_REQUIRE(std::get<bh61::dsp::DecodedBurst>(result).frame_bytes ==
               std::vector<std::uint8_t>(exact_type11_frame.begin(),
                                         exact_type11_frame.end()));
}

}  // namespace

BH61_TEST("acquisition recovers start phase carrier and conjugation") {
  constexpr std::uint32_t sample_rate = 4'000'000U;
  const auto waveform = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, sample_rate,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  const auto samples = impaired(waveform.samples, sample_rate, 173U, 0.71,
                                1'000.0, true);
  bh61::dsp::DecodeOptions options;
  options.carrier_hypotheses_hz = {-2'000.0, -1'000.0, 0.0,
                                    1'000.0, 2'000.0};
  const auto result =
      bh61::dsp::decode_first_burst(samples, sample_rate, options);
  BH61_REQUIRE(std::holds_alternative<bh61::dsp::DecodedBurst>(result));
  const auto& decoded = std::get<bh61::dsp::DecodedBurst>(result);
  BH61_REQUIRE(decoded.frame_bytes ==
               std::vector<std::uint8_t>(exact_type11_frame.begin(),
                                         exact_type11_frame.end()));
  BH61_REQUIRE(decoded.acquisition.start_sample == 173U);
  BH61_REQUIRE(decoded.acquisition.conjugated);
  BH61_REQUIRE(std::abs(decoded.acquisition.carrier_offset_hz + 1'000.0) < 1.0);
}

BH61_TEST("acquisition rejects silence instead of fabricating a frame") {
  const std::vector<std::complex<float>> silence(14'000, {0.0F, 0.0F});
  const auto result =
      bh61::dsp::decode_first_burst(silence, 4'000'000U, {});
  BH61_REQUIRE(std::holds_alternative<bh61::dsp::DecodeError>(result));
  BH61_REQUIRE(std::get<bh61::dsp::DecodeError>(result).code ==
               bh61::dsp::DecodeErrorCode::NoAcquisition);
}

BH61_TEST("finite hypotheses recover IQ swap and chip polarity") {
  constexpr std::uint32_t sample_rate = 4'000'000U;
  const auto swapped = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, sample_rate,
      bh61::dsp::OqpskOrientation::EvenChipsOnQ,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  require_exact_decode(swapped.samples, sample_rate);

  const auto inverted = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, sample_rate,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsNegative);
  require_exact_decode(inverted.samples, sample_rate);
}

BH61_TEST("decoder survives declared noise fractional timing and clock bounds") {
  constexpr std::uint32_t sample_rate = 4'000'000U;
  const auto waveform = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, sample_rate,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);

  auto clock_shifted = resample_clock(waveform.samples, 1.0001);
  std::vector<std::complex<float>> fractionally_shifted;
  fractionally_shifted.reserve(clock_shifted.size() + 1U);
  fractionally_shifted.push_back({0.0F, 0.0F});
  for (std::size_t index = 0; index < clock_shifted.size(); ++index) {
    const auto previous = index == 0U ? std::complex<float>{}
                                     : clock_shifted[index - 1U];
    fractionally_shifted.push_back(previous * 0.35F +
                                   clock_shifted[index] * 0.65F);
  }
  const auto noisy = add_deterministic_noise(fractionally_shifted, 0.04F);
  require_exact_decode(noisy, sample_rate);
}

BH61_TEST("decoder selects the earliest complete burst") {
  constexpr std::uint32_t sample_rate = 4'000'000U;
  const auto waveform = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, sample_rate,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  std::vector<std::complex<float>> capture(91U, {0.0F, 0.0F});
  capture.insert(capture.end(), waveform.samples.begin(),
                 waveform.samples.end());
  capture.insert(capture.end(), 317U, {0.0F, 0.0F});
  capture.insert(capture.end(), waveform.samples.begin(),
                 waveform.samples.end());

  const auto result = bh61::dsp::decode_first_burst(capture, sample_rate, {});
  BH61_REQUIRE(std::holds_alternative<bh61::dsp::DecodedBurst>(result));
  BH61_REQUIRE(std::get<bh61::dsp::DecodedBurst>(result)
                   .acquisition.start_sample == 91U);
}
