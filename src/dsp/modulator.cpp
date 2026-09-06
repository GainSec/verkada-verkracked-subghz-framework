#include "bh61/dsp/modulator.hpp"

#include "bh61/dsp/dsss.hpp"
#include "bh61/dsp/profile.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace bh61::dsp {
namespace {

auto chip_amplitude(std::uint8_t chip, ChipPolarity polarity) -> double {
  const auto canonical = chip == 0U ? 1.0 : -1.0;
  return polarity == ChipPolarity::ZeroIsPositive ? canonical : -canonical;
}

}  // namespace

auto modulate_oqpsk(std::span<const std::uint8_t> frame_bytes,
                    std::uint32_t sample_rate,
                    OqpskOrientation orientation, ChipPolarity polarity)
    -> ComplexWaveform {
  if (sample_rate < phy::chip_rate * 2U) {
    throw std::invalid_argument(
        "analytical OQPSK waveform requires at least two samples per chip");
  }

  const auto symbols = phy::burst_symbols(frame_bytes);
  std::vector<std::uint8_t> chips;
  chips.reserve(symbols.size() * 32U);
  for (const auto symbol : symbols) {
    const auto sequence = dsss_chips(symbol);
    chips.insert(chips.end(), sequence.begin(), sequence.end());
  }

  const auto duration = static_cast<double>(chips.size()) /
                        static_cast<double>(phy::chip_rate);
  const auto sample_count = static_cast<std::size_t>(std::llround(
      duration * static_cast<double>(sample_rate)));
  const auto chip_period = 1.0 / static_cast<double>(phy::chip_rate);
  const auto branch_period = 2.0 * chip_period;
  std::vector<std::complex<float>> samples;
  samples.reserve(sample_count);

  for (std::size_t sample_index = 0; sample_index < sample_count;
       ++sample_index) {
    const auto time =
        (static_cast<double>(sample_index) + 0.5) /
        static_cast<double>(sample_rate);

    const auto even_pair = static_cast<std::size_t>(time / branch_period);
    const auto even_chip = even_pair * 2U;
    double in_phase = 0.0;
    if (even_chip < chips.size()) {
      const auto local_time =
          time - static_cast<double>(even_chip) * chip_period;
      in_phase = chip_amplitude(chips[even_chip], polarity) *
                 std::sin(std::numbers::pi * local_time / branch_period);
    }

    double quadrature = 0.0;
    if (time >= chip_period) {
      const auto odd_pair =
          static_cast<std::size_t>((time - chip_period) / branch_period);
      const auto odd_chip = odd_pair * 2U + 1U;
      if (odd_chip < chips.size()) {
        const auto local_time =
            time - static_cast<double>(odd_chip) * chip_period;
        quadrature = chip_amplitude(chips[odd_chip], polarity) *
                     std::sin(std::numbers::pi * local_time / branch_period);
      }
    }

    const auto i = static_cast<float>(in_phase);
    const auto q = static_cast<float>(quadrature);
    samples.emplace_back(orientation == OqpskOrientation::EvenChipsOnI ? i : q,
                         orientation == OqpskOrientation::EvenChipsOnI ? q : i);
  }

  return ComplexWaveform{WaveformModel::AnalyticHalfSine,
                         orientation,
                         polarity,
                         false,
                         sample_rate,
                         symbols.size(),
                         chips.size(),
                         duration,
                         std::move(samples)};
}

}  // namespace bh61::dsp
