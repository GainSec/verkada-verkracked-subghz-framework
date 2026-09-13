#pragma once

#include "bh61/core/frame.hpp"
#include "bh61/dsp/acquisition.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bh61::dsp {

enum class DecodeErrorCode {
  NoAcquisition,
  Truncated,
  InvalidTraining,
  InvalidSymbol,
  InvalidFrame,
};

struct DecodeError {
  DecodeErrorCode code{};
  std::size_t offset{};
  std::string reason;
};

struct DecodeOptions {
  std::vector<double> carrier_hypotheses_hz{0.0};
  double minimum_acquisition_score{0.80};
  std::size_t maximum_symbol_distance{12};
  std::size_t coarse_start_stride{4};
  std::size_t coarse_template_stride{16};
  std::size_t maximum_coarse_regions{8};
  double maximum_sample_clock_correction_ppm{1'000.0};
  double sample_clock_correction_step_ppm{25.0};
  std::size_t maximum_timing_correction_samples{4U};
};

struct DecodedBurst {
  std::vector<std::uint8_t> frame_bytes;
  AcquisitionHypothesis acquisition;
  core::RadioFrame radio_frame;
};

struct DecodedFramePrefix {
  std::uint8_t phr{};
  std::vector<std::uint8_t> psdu_prefix;
  AcquisitionHypothesis acquisition;
};

using DecodeResult = std::variant<DecodedBurst, DecodeError>;
using FramePrefixResult = std::variant<DecodedFramePrefix, DecodeError>;

auto decode_first_burst(std::span<const std::complex<float>> samples,
                        std::uint32_t sample_rate,
                        const DecodeOptions& options) -> DecodeResult;

auto decode_bursts(std::span<const std::complex<float>> samples,
                   std::uint32_t sample_rate, const DecodeOptions& options,
                   std::size_t maximum_bursts) -> std::vector<DecodedBurst>;

auto decode_frame_prefix(std::span<const std::complex<float>> samples,
                         std::uint32_t sample_rate,
                         const DecodeOptions& options,
                         std::size_t psdu_prefix_bytes) -> FramePrefixResult;

}  // namespace bh61::dsp
