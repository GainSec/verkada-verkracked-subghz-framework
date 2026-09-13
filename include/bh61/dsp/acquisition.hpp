#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bh61::dsp {

struct AcquisitionOptions {
  std::vector<double> carrier_hypotheses_hz{0.0};
  double minimum_score{0.80};
  double equivalent_score_epsilon{1e-9};
  std::size_t coarse_start_stride{4};
  std::size_t coarse_template_stride{16};
  std::size_t maximum_coarse_regions{8};
};

struct AcquisitionHypothesis {
  std::size_t start_sample{};
  bool conjugated{};
  double carrier_offset_hz{};
  double phase_radians{};
  double score{};
  double runner_up_score{};
  double sample_clock_correction_ppm{};
};

struct AcquisitionError {
  std::string reason;
};

using AcquisitionResult =
    std::variant<AcquisitionHypothesis, AcquisitionError>;
using AcquisitionCandidatesResult =
    std::variant<std::vector<AcquisitionHypothesis>, AcquisitionError>;

auto acquire_training_candidates(
    std::span<const std::complex<float>> samples,
    std::uint32_t sample_rate,
    const AcquisitionOptions& options) -> AcquisitionCandidatesResult;

auto acquire_training(std::span<const std::complex<float>> samples,
                      std::uint32_t sample_rate,
                      const AcquisitionOptions& options)
    -> AcquisitionResult;

}  // namespace bh61::dsp
