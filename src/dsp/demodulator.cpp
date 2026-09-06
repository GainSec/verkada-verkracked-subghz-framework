#include "bh61/dsp/demodulator.hpp"

#include "bh61/dsp/dsss.hpp"
#include "bh61/dsp/profile.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>

namespace bh61::dsp {
namespace {

auto corrected_sample(std::span<const std::complex<float>> samples,
                      std::uint32_t sample_rate,
                      const AcquisitionHypothesis& hypothesis,
                      double absolute_sample_position) -> std::complex<double> {
  const auto lower = static_cast<std::size_t>(
      std::floor(absolute_sample_position));
  const auto fraction = absolute_sample_position - static_cast<double>(lower);
  if (lower >= samples.size()) {
    return {};
  }
  auto first = static_cast<std::complex<double>>(samples[lower]);
  auto second = first;
  if (lower + 1U < samples.size()) {
    second = static_cast<std::complex<double>>(samples[lower + 1U]);
  }
  if (hypothesis.conjugated) {
    first = std::conj(first);
    second = std::conj(second);
  }
  auto value = first * (1.0 - fraction) + second * fraction;
  const auto angle =
      2.0 * std::numbers::pi * hypothesis.carrier_offset_hz *
          absolute_sample_position / static_cast<double>(sample_rate) -
      hypothesis.phase_radians;
  value *= std::complex<double>{std::cos(angle), std::sin(angle)};
  return value;
}

auto decode_symbol(std::span<const std::complex<float>> samples,
                   std::uint32_t sample_rate,
                   const AcquisitionHypothesis& hypothesis,
                   std::size_t symbol_index, std::size_t maximum_distance)
    -> std::variant<std::uint8_t, DecodeError> {
  std::array<std::uint8_t, 32> chips{};
  const auto samples_per_chip = static_cast<double>(sample_rate) /
                                static_cast<double>(phy::chip_rate);
  for (std::size_t chip = 0; chip < chips.size(); ++chip) {
    const auto absolute_chip = symbol_index * chips.size() + chip;
    const auto position = static_cast<double>(hypothesis.start_sample) +
                          (static_cast<double>(absolute_chip) + 1.0) *
                              samples_per_chip -
                          0.5;
    if (position < 0.0 || position >= static_cast<double>(samples.size())) {
      return DecodeError{DecodeErrorCode::Truncated, absolute_chip,
                         "capture ends before the requested chip center"};
    }
    const auto value =
        corrected_sample(samples, sample_rate, hypothesis, position);
    const auto decision = chip % 2U == 0U ? value.real() : value.imag();
    chips[chip] = decision >= 0.0 ? 0U : 1U;
  }

  const auto decoded = decode_hard(chips);
  if (decoded.ambiguous || decoded.distance > maximum_distance) {
    return DecodeError{DecodeErrorCode::InvalidSymbol, symbol_index,
                       decoded.ambiguous
                           ? "DSSS decision is tied"
                           : "DSSS decision exceeds distance threshold"};
  }
  return decoded.symbol;
}

}  // namespace

auto decode_first_burst(std::span<const std::complex<float>> samples,
                        std::uint32_t sample_rate,
                        const DecodeOptions& options) -> DecodeResult {
  AcquisitionOptions acquisition_options;
  acquisition_options.carrier_hypotheses_hz =
      options.carrier_hypotheses_hz;
  acquisition_options.minimum_score = options.minimum_acquisition_score;
  const auto acquisition =
      acquire_training(samples, sample_rate, acquisition_options);
  if (std::holds_alternative<AcquisitionError>(acquisition)) {
    return DecodeError{DecodeErrorCode::NoAcquisition, 0,
                       std::get<AcquisitionError>(acquisition).reason};
  }
  const auto hypothesis = std::get<AcquisitionHypothesis>(acquisition);

  constexpr auto training_symbols =
      phy::preamble_symbols + phy::sync_symbols.size();
  for (std::size_t symbol_index = 0; symbol_index < training_symbols;
       ++symbol_index) {
    const auto decoded = decode_symbol(samples, sample_rate, hypothesis,
                                       symbol_index,
                                       options.maximum_symbol_distance);
    if (std::holds_alternative<DecodeError>(decoded)) {
      return std::get<DecodeError>(decoded);
    }
    const auto expected = symbol_index < phy::preamble_symbols
                              ? 0U
                              : phy::sync_symbols[symbol_index -
                                                  phy::preamble_symbols];
    if (std::get<std::uint8_t>(decoded) != expected) {
      return DecodeError{DecodeErrorCode::InvalidTraining, symbol_index,
                         "decoded training symbol does not match profile"};
    }
  }

  std::array<std::uint8_t, 2> phr_symbols{};
  for (std::size_t index = 0; index < phr_symbols.size(); ++index) {
    const auto decoded = decode_symbol(
        samples, sample_rate, hypothesis, training_symbols + index,
        options.maximum_symbol_distance);
    if (std::holds_alternative<DecodeError>(decoded)) {
      return std::get<DecodeError>(decoded);
    }
    phr_symbols[index] = std::get<std::uint8_t>(decoded);
  }
  const auto phr = static_cast<std::uint8_t>(phr_symbols[0] |
                                             (phr_symbols[1] << 4U));
  if (phr < 5U || phr > 127U) {
    return DecodeError{DecodeErrorCode::InvalidFrame, training_symbols,
                       "decoded PHR is outside the recovered range"};
  }

  const auto frame_size = static_cast<std::size_t>(phr) + 1U;
  std::vector<std::uint8_t> frame_bytes(frame_size, 0U);
  for (std::size_t byte_index = 0; byte_index < frame_size; ++byte_index) {
    std::array<std::uint8_t, 2> symbols{};
    for (std::size_t nibble = 0; nibble < symbols.size(); ++nibble) {
      const auto symbol_index = training_symbols + byte_index * 2U + nibble;
      const auto decoded = decode_symbol(samples, sample_rate, hypothesis,
                                         symbol_index,
                                         options.maximum_symbol_distance);
      if (std::holds_alternative<DecodeError>(decoded)) {
        return std::get<DecodeError>(decoded);
      }
      symbols[nibble] = std::get<std::uint8_t>(decoded);
    }
    frame_bytes[byte_index] = static_cast<std::uint8_t>(
        symbols[0] | static_cast<std::uint8_t>(symbols[1] << 4U));
  }

  const auto parsed = core::parse_radio_frame(frame_bytes);
  if (std::holds_alternative<core::ParseError>(parsed)) {
    const auto& error = std::get<core::ParseError>(parsed);
    return DecodeError{DecodeErrorCode::InvalidFrame, error.offset,
                       error.layer + ": " + error.reason};
  }
  return DecodedBurst{std::move(frame_bytes), hypothesis,
                      std::get<core::RadioFrame>(parsed)};
}

}  // namespace bh61::dsp
