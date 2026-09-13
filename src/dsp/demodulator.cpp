#include "bh61/dsp/demodulator.hpp"

#include "bh61/dsp/dsss.hpp"
#include "bh61/dsp/profile.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
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
                   std::size_t symbol_index, double sample_clock_scale,
                   std::size_t maximum_distance)
    -> std::variant<std::uint8_t, DecodeError> {
  std::array<std::uint8_t, 32> chips{};
  std::array<double, 32> chip_confidence{};
  const auto samples_per_chip =
      static_cast<double>(sample_rate) /
      static_cast<double>(phy::chip_rate) * sample_clock_scale;
  for (std::size_t chip = 0; chip < chip_confidence.size(); ++chip) {
    const auto absolute_chip = symbol_index * chip_confidence.size() + chip;
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
    chip_confidence[chip] = decision;
    chips[chip] = decision >= 0.0 ? 0U : 1U;
  }

  const auto hard = decode_hard(chips);
  const auto soft = decode_soft(chip_confidence);
  if (soft.ambiguous || soft.hard_distance > maximum_distance) {
    return DecodeError{DecodeErrorCode::InvalidSymbol, symbol_index,
                       soft.ambiguous
                           ? "soft DSSS decision is tied"
                           : "DSSS decision exceeds distance threshold"};
  }
  if (maximum_distance <= 12U && !hard.ambiguous &&
      hard.distance <= maximum_distance && hard.symbol != soft.symbol) {
    return DecodeError{DecodeErrorCode::InvalidSymbol, symbol_index,
                       "hard and soft DSSS decisions disagree"};
  }
  return soft.symbol;
}

auto decode_at_sample_clock(
    std::span<const std::complex<float>> samples, std::uint32_t sample_rate,
    AcquisitionHypothesis hypothesis, const DecodeOptions& options,
    double sample_clock_correction_ppm) -> DecodeResult {
  const auto sample_clock_scale =
      1.0 + sample_clock_correction_ppm / 1'000'000.0;
  hypothesis.sample_clock_correction_ppm = sample_clock_correction_ppm;

  constexpr auto training_symbols =
      phy::preamble_symbols + phy::sync_symbols.size();
  for (std::size_t symbol_index = 0; symbol_index < training_symbols;
       ++symbol_index) {
    const auto decoded = decode_symbol(
        samples, sample_rate, hypothesis, symbol_index, sample_clock_scale,
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
        sample_clock_scale, options.maximum_symbol_distance);
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
      const auto decoded = decode_symbol(
          samples, sample_rate, hypothesis, symbol_index, sample_clock_scale,
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

auto decode_acquisition_hypothesis(
    std::span<const std::complex<float>> samples, std::uint32_t sample_rate,
    const AcquisitionHypothesis& hypothesis, const DecodeOptions& options)
    -> DecodeResult {
  std::optional<DecodedBurst> winner;
  std::optional<DecodeError> nominal_error;
  const auto step_count = static_cast<std::size_t>(std::floor(
      options.maximum_sample_clock_correction_ppm /
      options.sample_clock_correction_step_ppm));
  for (std::size_t timing_step = 0U;
       timing_step <= options.maximum_timing_correction_samples;
       ++timing_step) {
    const std::array<std::int64_t, 2> timing_offsets{
        static_cast<std::int64_t>(timing_step),
        -static_cast<std::int64_t>(timing_step)};
    const auto timing_count = timing_step == 0U ? 1U : timing_offsets.size();
    for (std::size_t timing_index = 0U; timing_index < timing_count;
         ++timing_index) {
      for (std::size_t step = 0U; step <= step_count; ++step) {
        const auto magnitude =
            static_cast<double>(step) * options.sample_clock_correction_step_ppm;
        const std::array<double, 2> corrections{magnitude, -magnitude};
        const auto correction_count = step == 0U ? 1U : corrections.size();
        for (std::size_t index = 0U; index < correction_count; ++index) {
          const auto corrected_start =
              static_cast<std::int64_t>(hypothesis.start_sample) +
              timing_offsets[timing_index];
          if (corrected_start < 0) continue;
          auto timed_hypothesis = hypothesis;
          timed_hypothesis.start_sample =
              static_cast<std::size_t>(corrected_start);
          auto candidate = decode_at_sample_clock(
              samples, sample_rate, timed_hypothesis, options,
              corrections[index]);
          if (auto* decoded = std::get_if<DecodedBurst>(&candidate)) {
            if (winner.has_value() &&
                winner->frame_bytes != decoded->frame_bytes) {
              return DecodeError{
                  DecodeErrorCode::InvalidFrame, 0,
                  "timing/sample-clock hypotheses yielded different valid frames"};
            }
            if (!winner.has_value()) winner = std::move(*decoded);
          } else if (step == 0U && timing_step == 0U &&
                     timing_index == 0U) {
            nominal_error = std::get<DecodeError>(std::move(candidate));
          }
        }
      }
    }
    if (winner.has_value()) return std::move(*winner);
  }
  return *nominal_error;
}

}  // namespace

auto decode_first_burst(std::span<const std::complex<float>> samples,
                        std::uint32_t sample_rate,
                        const DecodeOptions& options) -> DecodeResult {
  if (!std::isfinite(options.maximum_sample_clock_correction_ppm) ||
      !std::isfinite(options.sample_clock_correction_step_ppm) ||
      options.maximum_sample_clock_correction_ppm < 0.0 ||
      options.maximum_sample_clock_correction_ppm > 10'000.0 ||
      options.sample_clock_correction_step_ppm <= 0.0 ||
      options.maximum_timing_correction_samples > 64U) {
    return DecodeError{DecodeErrorCode::NoAcquisition, 0,
                       "sample-clock search bounds are invalid"};
  }
  AcquisitionOptions acquisition_options;
  acquisition_options.carrier_hypotheses_hz =
      options.carrier_hypotheses_hz;
  acquisition_options.minimum_score = options.minimum_acquisition_score;
  acquisition_options.coarse_start_stride = options.coarse_start_stride;
  acquisition_options.coarse_template_stride =
      options.coarse_template_stride;
  acquisition_options.maximum_coarse_regions =
      options.maximum_coarse_regions;
  const auto acquisitions =
      acquire_training_candidates(samples, sample_rate, acquisition_options);
  if (std::holds_alternative<AcquisitionError>(acquisitions)) {
    return DecodeError{DecodeErrorCode::NoAcquisition, 0,
                       std::get<AcquisitionError>(acquisitions).reason};
  }

  std::optional<DecodedBurst> earliest_valid;
  std::optional<DecodeError> best_region_error;
  double best_error_score = -std::numeric_limits<double>::infinity();
  for (const auto& hypothesis :
       std::get<std::vector<AcquisitionHypothesis>>(acquisitions)) {
    auto decoded = decode_acquisition_hypothesis(
        samples, sample_rate, hypothesis, options);
    if (auto* burst = std::get_if<DecodedBurst>(&decoded)) {
      if (!earliest_valid.has_value() ||
          burst->acquisition.start_sample <
              earliest_valid->acquisition.start_sample) {
        earliest_valid = std::move(*burst);
      }
    } else if (hypothesis.score > best_error_score) {
      best_error_score = hypothesis.score;
      best_region_error = std::get<DecodeError>(std::move(decoded));
    }
  }
  if (earliest_valid.has_value()) return std::move(*earliest_valid);
  return *best_region_error;
}

auto decode_frame_prefix(std::span<const std::complex<float>> samples,
                         std::uint32_t sample_rate,
                         const DecodeOptions& options,
                         std::size_t psdu_prefix_bytes) -> FramePrefixResult {
  if (sample_rate == 0U || psdu_prefix_bytes == 0U ||
      psdu_prefix_bytes > 125U ||
      !std::isfinite(options.maximum_sample_clock_correction_ppm) ||
      !std::isfinite(options.sample_clock_correction_step_ppm) ||
      options.maximum_sample_clock_correction_ppm < 0.0 ||
      options.maximum_sample_clock_correction_ppm > 10'000.0 ||
      options.sample_clock_correction_step_ppm <= 0.0) {
    return DecodeError{DecodeErrorCode::InvalidFrame, 0U,
                       "frame-prefix bounds are invalid"};
  }
  AcquisitionOptions acquisition_options;
  acquisition_options.carrier_hypotheses_hz =
      options.carrier_hypotheses_hz;
  acquisition_options.minimum_score = options.minimum_acquisition_score;
  acquisition_options.coarse_start_stride = options.coarse_start_stride;
  acquisition_options.coarse_template_stride =
      options.coarse_template_stride;
  acquisition_options.maximum_coarse_regions =
      options.maximum_coarse_regions;
  const auto acquired =
      acquire_training(samples, sample_rate, acquisition_options);
  if (const auto* error = std::get_if<AcquisitionError>(&acquired)) {
    return DecodeError{DecodeErrorCode::NoAcquisition, 0U, error->reason};
  }
  const auto acquired_hypothesis = std::get<AcquisitionHypothesis>(acquired);
  constexpr auto training_symbols =
      phy::preamble_symbols + phy::sync_symbols.size();
  const auto decode_at_correction = [&](double correction_ppm)
      -> FramePrefixResult {
    auto hypothesis = acquired_hypothesis;
    hypothesis.sample_clock_correction_ppm = correction_ppm;
    const auto sample_clock_scale = 1.0 + correction_ppm / 1'000'000.0;
    for (std::size_t symbol_index = 0U; symbol_index < training_symbols;
         ++symbol_index) {
      const auto decoded = decode_symbol(
          samples, sample_rate, hypothesis, symbol_index, sample_clock_scale,
          options.maximum_symbol_distance);
      if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
      const auto expected = symbol_index < phy::preamble_symbols
                                ? 0U
                                : phy::sync_symbols[symbol_index -
                                                    phy::preamble_symbols];
      if (std::get<std::uint8_t>(decoded) != expected) {
        return DecodeError{DecodeErrorCode::InvalidTraining, symbol_index,
                           "decoded training symbol does not match profile"};
      }
    }

    const auto decode_byte = [&](std::size_t frame_byte_index)
        -> std::variant<std::uint8_t, DecodeError> {
      std::array<std::uint8_t, 2> symbols{};
      for (std::size_t nibble = 0U; nibble < symbols.size(); ++nibble) {
        const auto symbol_index =
            training_symbols + frame_byte_index * 2U + nibble;
        const auto decoded = decode_symbol(
            samples, sample_rate, hypothesis, symbol_index, sample_clock_scale,
            options.maximum_symbol_distance);
        if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
        symbols[nibble] = std::get<std::uint8_t>(decoded);
      }
      return static_cast<std::uint8_t>(symbols[0] | (symbols[1] << 4U));
    };

    const auto decoded_phr = decode_byte(0U);
    if (const auto* error = std::get_if<DecodeError>(&decoded_phr)) return *error;
    const auto phr = std::get<std::uint8_t>(decoded_phr);
    if (phr < 5U || phr > 127U || psdu_prefix_bytes > phr - 2U) {
      return DecodeError{DecodeErrorCode::InvalidFrame, training_symbols,
                         "decoded PHR cannot contain the requested prefix"};
    }
    std::vector<std::uint8_t> prefix;
    prefix.reserve(psdu_prefix_bytes);
    for (std::size_t index = 0U; index < psdu_prefix_bytes; ++index) {
      const auto decoded = decode_byte(index + 1U);
      if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
      prefix.push_back(std::get<std::uint8_t>(decoded));
    }
    return DecodedFramePrefix{phr, std::move(prefix), hypothesis};
  };

  std::optional<DecodedFramePrefix> winner;
  std::optional<DecodeError> nominal_error;
  const auto step_count = static_cast<std::size_t>(std::floor(
      options.maximum_sample_clock_correction_ppm /
      options.sample_clock_correction_step_ppm));
  for (std::size_t step = 0U; step <= step_count; ++step) {
    const auto magnitude =
        static_cast<double>(step) * options.sample_clock_correction_step_ppm;
    const std::array<double, 2> corrections{magnitude, -magnitude};
    const auto correction_count = step == 0U ? 1U : corrections.size();
    for (std::size_t index = 0U; index < correction_count; ++index) {
      auto candidate = decode_at_correction(corrections[index]);
      if (auto* decoded = std::get_if<DecodedFramePrefix>(&candidate)) {
        if (winner && (winner->phr != decoded->phr ||
                       winner->psdu_prefix != decoded->psdu_prefix)) {
          return DecodeError{DecodeErrorCode::InvalidFrame, 0U,
                             "sample-clock hypotheses yielded different prefixes"};
        }
        if (!winner) winner = std::move(*decoded);
      } else if (step == 0U) {
        nominal_error = std::get<DecodeError>(std::move(candidate));
      }
    }
  }
  if (winner) return std::move(*winner);
  return *nominal_error;
}

auto decode_bursts(std::span<const std::complex<float>> samples,
                   std::uint32_t sample_rate, const DecodeOptions& options,
                   std::size_t maximum_bursts) -> std::vector<DecodedBurst> {
  if (sample_rate == 0U || maximum_bursts == 0U) {
    throw std::invalid_argument("multi-burst decode bounds must be positive");
  }
  std::vector<DecodedBurst> bursts;
  std::size_t cursor = 0U;
  while (cursor < samples.size() && bursts.size() < maximum_bursts) {
    const auto decoded = decode_first_burst(samples.subspan(cursor),
                                            sample_rate, options);
    const auto* burst = std::get_if<DecodedBurst>(&decoded);
    if (burst == nullptr) break;

    auto absolute = *burst;
    absolute.acquisition.start_sample += cursor;
    constexpr auto training_symbols =
        phy::preamble_symbols + phy::sync_symbols.size();
    const auto total_symbols =
        training_symbols + absolute.frame_bytes.size() * 2U;
    if (total_symbols >
        std::numeric_limits<std::size_t>::max() / sample_rate) {
      throw std::overflow_error("decoded burst sample extent overflows");
    }
    const auto numerator = total_symbols * sample_rate;
    const auto burst_samples =
        (numerator + phy::symbol_rate - 1U) / phy::symbol_rate;
    const auto consumed = burst->acquisition.start_sample + burst_samples;
    bursts.push_back(std::move(absolute));
    if (consumed == 0U || consumed > samples.size() - cursor) break;
    cursor += consumed;
  }
  return bursts;
}

}  // namespace bh61::dsp
