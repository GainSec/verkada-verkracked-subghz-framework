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
  std::size_t maximum_symbol_distance{8};
};

struct DecodedBurst {
  std::vector<std::uint8_t> frame_bytes;
  AcquisitionHypothesis acquisition;
  core::RadioFrame radio_frame;
};

using DecodeResult = std::variant<DecodedBurst, DecodeError>;

auto decode_first_burst(std::span<const std::complex<float>> samples,
                        std::uint32_t sample_rate,
                        const DecodeOptions& options) -> DecodeResult;

}  // namespace bh61::dsp
