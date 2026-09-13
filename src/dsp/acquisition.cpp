#include "bh61/dsp/acquisition.hpp"

#include "bh61/dsp/modulator.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace bh61::dsp {
namespace {

struct ScoredCandidate {
  std::size_t start{};
  bool conjugated{};
  double carrier_correction_hz{};
  double phase{};
  double score{-1.0};
};

auto score_candidate(std::span<const std::complex<float>> samples,
                     std::span<const std::complex<float>> training,
                     std::uint32_t sample_rate, std::size_t start,
                     bool conjugated, double carrier_correction_hz,
                     std::size_t template_stride) -> ScoredCandidate {
  std::complex<double> correlation{};
  double sample_energy = 0.0;
  double training_energy = 0.0;
  const auto rate = static_cast<double>(sample_rate);
  for (std::size_t offset = 0; offset < training.size();
       offset += template_stride) {
    auto value = static_cast<std::complex<double>>(samples[start + offset]);
    if (conjugated) {
      value = std::conj(value);
    }
    const auto absolute_index = static_cast<double>(start + offset);
    const auto angle = 2.0 * std::numbers::pi * carrier_correction_hz *
                       absolute_index / rate;
    value *= std::complex<double>{std::cos(angle), std::sin(angle)};
    const auto reference =
        static_cast<std::complex<double>>(training[offset]);
    correlation += std::conj(reference) * value;
    sample_energy += std::norm(value);
    training_energy += std::norm(reference);
  }

  const auto denominator = std::sqrt(sample_energy * training_energy);
  if (denominator <= std::numeric_limits<double>::epsilon()) {
    return ScoredCandidate{start, conjugated, carrier_correction_hz, 0.0,
                           0.0};
  }
  return ScoredCandidate{start, conjugated, carrier_correction_hz,
                         std::arg(correlation),
                         std::abs(correlation) / denominator};
}

auto better_scored_candidate(const ScoredCandidate& left,
                             const ScoredCandidate& right)
    -> bool {
  if (left.score != right.score) {
    return left.score > right.score;
  }
  if (left.start != right.start) {
    return left.start < right.start;
  }
  if (left.conjugated != right.conjugated) {
    return !left.conjugated;
  }
  return left.carrier_correction_hz < right.carrier_correction_hz;
}

auto better_hypothesis(const AcquisitionHypothesis& left,
                       const AcquisitionHypothesis& right) -> bool {
  if (left.score != right.score) {
    return left.score > right.score;
  }
  if (left.start_sample != right.start_sample) {
    return left.start_sample < right.start_sample;
  }
  if (left.conjugated != right.conjugated) {
    return !left.conjugated;
  }
  return left.carrier_offset_hz < right.carrier_offset_hz;
}

}  // namespace

auto acquire_training_candidates(
    std::span<const std::complex<float>> samples,
    std::uint32_t sample_rate,
    const AcquisitionOptions& options) -> AcquisitionCandidatesResult {
  if (sample_rate == 0U) {
    return AcquisitionError{"sample rate must be nonzero"};
  }
  if (options.carrier_hypotheses_hz.empty()) {
    return AcquisitionError{"carrier hypothesis grid must not be empty"};
  }
  if (options.coarse_start_stride == 0U ||
      options.coarse_template_stride == 0U ||
      options.maximum_coarse_regions == 0U) {
    return AcquisitionError{"acquisition strides must be nonzero"};
  }

  const auto training_waveform = modulate_oqpsk(
      {}, sample_rate, OqpskOrientation::EvenChipsOnI,
      ChipPolarity::ZeroIsPositive);
  const auto training = std::span<const std::complex<float>>(
      training_waveform.samples.data(), training_waveform.samples.size());
  if (samples.size() < training.size()) {
    return AcquisitionError{"capture is shorter than the training sequence"};
  }

  std::vector<ScoredCandidate> coarse_candidates;
  const auto last_start = samples.size() - training.size();
  for (const auto carrier : options.carrier_hypotheses_hz) {
    for (const auto conjugated : {false, true}) {
      for (std::size_t start = 0; start <= last_start;
           start += options.coarse_start_stride) {
        const auto candidate = score_candidate(
            samples, training, sample_rate, start, conjugated, carrier,
            options.coarse_template_stride);
        coarse_candidates.push_back(candidate);
      }
    }
  }

  std::sort(coarse_candidates.begin(), coarse_candidates.end(),
            better_scored_candidate);
  std::vector<ScoredCandidate> coarse_seeds;
  const auto region_separation = training.size() / 2U;
  for (const auto& candidate : coarse_candidates) {
    const auto overlaps = std::any_of(
        coarse_seeds.begin(), coarse_seeds.end(),
        [&](const ScoredCandidate& seed) {
          const auto distance = candidate.start > seed.start
                                    ? candidate.start - seed.start
                                    : seed.start - candidate.start;
          return distance < region_separation;
        });
    if (!overlaps) {
      coarse_seeds.push_back(candidate);
      if (coarse_seeds.size() == options.maximum_coarse_regions) {
        break;
      }
    }
  }

  const auto radius = options.coarse_start_stride;
  std::vector<AcquisitionHypothesis> hypotheses;
  for (const auto& seed : coarse_seeds) {
    std::vector<ScoredCandidate> refined;
    const auto first_refined =
        seed.start > radius ? seed.start - radius : 0U;
    const auto last_refined = std::min(last_start, seed.start + radius);
    for (const auto carrier : options.carrier_hypotheses_hz) {
      for (const auto conjugated : {false, true}) {
        for (std::size_t start = first_refined; start <= last_refined;
             ++start) {
          refined.push_back(score_candidate(samples, training, sample_rate,
                                            start, conjugated, carrier, 1U));
        }
      }
    }
    std::sort(refined.begin(), refined.end(), better_scored_candidate);
    if (refined.empty() || refined.front().score < options.minimum_score) {
      continue;
    }

    auto winner = refined.front();
    const auto equivalent_floor =
        winner.score - options.equivalent_score_epsilon;
    for (const auto& candidate : refined) {
      if (candidate.score < equivalent_floor) {
        break;
      }
      if (candidate.start < winner.start ||
          (candidate.start == winner.start &&
           better_scored_candidate(candidate, winner))) {
        winner = candidate;
      }
    }
    const auto runner_up = refined.size() > 1U ? refined[1].score : 0.0;
    hypotheses.push_back(AcquisitionHypothesis{
        winner.start, winner.conjugated, winner.carrier_correction_hz,
        winner.phase, winner.score, runner_up, 0.0});
  }
  if (hypotheses.empty()) {
    return AcquisitionError{"no training hypothesis met the score threshold"};
  }

  std::sort(hypotheses.begin(), hypotheses.end(),
            [](const AcquisitionHypothesis& left,
               const AcquisitionHypothesis& right) {
              return left.start_sample < right.start_sample;
            });
  return hypotheses;
}

auto acquire_training(std::span<const std::complex<float>> samples,
                      std::uint32_t sample_rate,
                      const AcquisitionOptions& options)
    -> AcquisitionResult {
  const auto acquired =
      acquire_training_candidates(samples, sample_rate, options);
  if (const auto* error = std::get_if<AcquisitionError>(&acquired)) {
    return *error;
  }

  auto hypotheses = std::get<std::vector<AcquisitionHypothesis>>(acquired);
  std::sort(hypotheses.begin(), hypotheses.end(), better_hypothesis);
  auto winner = hypotheses.front();

  const auto equivalent_floor =
      winner.score - options.equivalent_score_epsilon;
  for (const auto& candidate : hypotheses) {
    if (candidate.score < equivalent_floor) {
      break;
    }
    if (candidate.start_sample < winner.start_sample ||
        (candidate.start_sample == winner.start_sample &&
         better_hypothesis(candidate, winner))) {
      winner = candidate;
    }
  }
  winner.runner_up_score = hypotheses.size() > 1U ? hypotheses[1].score : 0.0;
  return winner;
}

}  // namespace bh61::dsp
