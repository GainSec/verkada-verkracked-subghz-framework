#include "bh61/dsp/modulator.hpp"

#include "bh61/dsp/dsss.hpp"
#include "bh61/dsp/profile.hpp"

#include <cmath>
#include <numeric>
#include <numbers>
#include <stdexcept>

namespace bh61::dsp {
namespace {

auto chip_amplitude(std::uint8_t chip, ChipPolarity polarity) -> double {
  const auto canonical = chip == 0U ? 1.0 : -1.0;
  return polarity == ChipPolarity::ZeroIsPositive ? canonical : -canonical;
}

auto bessel_i0(double value) -> double {
  const auto term_base = value * value / 4.0;
  double sum = 1.0;
  double term = 1.0;
  for (unsigned order = 1U; order < 64U; ++order) {
    term *= term_base /
            (static_cast<double>(order) * static_cast<double>(order));
    sum += term;
    if (term < sum * 1e-16) break;
  }
  return sum;
}

auto kaiser_window(std::size_t index, std::size_t length, double beta)
    -> double {
  const auto position =
      2.0 * static_cast<double>(index) / static_cast<double>(length - 1U) -
      1.0;
  return bessel_i0(
             beta * std::sqrt(std::max(0.0, 1.0 - position * position))) /
         bessel_i0(beta);
}

auto sinc(double value) -> double {
  if (std::abs(value) < 1e-15) return 1.0;
  const auto argument = std::numbers::pi * value;
  return std::sin(argument) / argument;
}

auto polyphase_resample(std::span<const std::complex<double>> input,
                        std::uint32_t source_rate,
                        std::uint32_t output_rate)
    -> std::vector<std::complex<float>> {
  const auto divisor = std::gcd(source_rate, output_rate);
  const auto up = output_rate / divisor;
  const auto down = source_rate / divisor;
  if (up == 1U && down == 1U) {
    std::vector<std::complex<float>> output;
    output.reserve(input.size());
    for (const auto sample : input) output.emplace_back(sample);
    return output;
  }

  // Match scipy.signal.resample_poly's default FIR: a 20*max_rate+1 tap
  // Kaiser(beta=5) low-pass filter, scaled after interpolation.
  const auto max_rate = std::max(up, down);
  if (max_rate > 512U) {
    throw std::invalid_argument(
        "EFR32 OQPSK sample-rate ratio is too large for bounded resampling");
  }
  const auto half_length = 10U * max_rate;
  const auto filter_length = 2U * half_length + 1U;
  const auto cutoff = 1.0 / static_cast<double>(max_rate);
  std::vector<double> filter(filter_length);
  double filter_sum = 0.0;
  for (std::size_t index = 0; index < filter.size(); ++index) {
    const auto offset = static_cast<double>(index) -
                        static_cast<double>(half_length);
    filter[index] = cutoff * sinc(cutoff * offset) *
                    kaiser_window(index, filter.size(), 5.0);
    filter_sum += filter[index];
  }
  for (auto& tap : filter) {
    tap = tap / filter_sum * static_cast<double>(up);
  }

  const auto pre_padding = down - (half_length % down);
  std::vector<double> padded(pre_padding, 0.0);
  padded.insert(padded.end(), filter.begin(), filter.end());
  const auto pre_remove = (half_length + pre_padding) / down;
  const auto output_count =
      (input.size() * static_cast<std::size_t>(up) + down - 1U) / down;

  std::vector<std::complex<float>> output;
  output.reserve(output_count);
  for (std::size_t output_index = 0; output_index < output_count;
       ++output_index) {
    const auto filtered_index = output_index + pre_remove;
    const auto position = filtered_index * static_cast<std::size_t>(down);
    const auto first_input =
        position >= padded.size() - 1U
            ? (position - (padded.size() - 1U) + up - 1U) / up
            : 0U;
    const auto last_input = std::min(
        input.size() - 1U, position / static_cast<std::size_t>(up));
    std::complex<double> value{};
    if (first_input <= last_input) {
      for (auto input_index = first_input; input_index <= last_input;
           ++input_index) {
        const auto tap_index =
            position - input_index * static_cast<std::size_t>(up);
        value += input[input_index] * padded[tap_index];
      }
    }
    output.emplace_back(value);
  }
  return output;
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

auto modulate_efr32_custom_oqpsk(
    std::span<const std::uint8_t> frame_bytes, std::uint32_t sample_rate)
    -> ComplexWaveform {
  if (sample_rate == 0U) {
    throw std::invalid_argument("EFR32 OQPSK sample rate must be positive");
  }

  const auto symbols = phy::burst_symbols(frame_bytes);
  std::vector<std::uint8_t> chips;
  chips.reserve(symbols.size() * 32U);
  for (const auto symbol : symbols) {
    const auto sequence = dsss_chips(symbol);
    chips.insert(chips.end(), sequence.begin(), sequence.end());
  }

  std::vector<double> direction(chips.size());
  const auto prior = chip_amplitude(dsss_chips(0U).back(),
                                    ChipPolarity::ZeroIsPositive);
  const auto first =
      chip_amplitude(chips.front(), ChipPolarity::ZeroIsPositive);
  direction.front() = -first * prior;
  for (std::size_t index = 1; index < chips.size(); ++index) {
    const auto current =
        chip_amplitude(chips[index], ChipPolarity::ZeroIsPositive);
    const auto previous =
        chip_amplitude(chips[index - 1U], ChipPolarity::ZeroIsPositive);
    direction[index] = (index % 2U == 1U ? 1.0 : -1.0) * current * previous;
  }

  constexpr std::array<double, 16> mode2_taps{
      1.0 / 128.0,   1.0 / 128.0,   16.0 / 128.0,  48.0 / 128.0,
      80.0 / 128.0,  112.0 / 128.0, 127.0 / 128.0, 127.0 / 128.0,
      127.0 / 128.0, 127.0 / 128.0, 112.0 / 128.0, 80.0 / 128.0,
      48.0 / 128.0,  16.0 / 128.0,  1.0 / 128.0,   1.0 / 128.0};
  std::vector<double> shaped(chips.size() * 8U, 0.0);
  for (std::size_t chip = 0; chip < direction.size(); ++chip) {
    const auto impulse = chip * 8U;
    for (std::size_t tap = 0;
         tap < mode2_taps.size() && impulse + tap < shaped.size(); ++tap) {
      shaped[impulse + tap] += direction[chip] * mode2_taps[tap];
    }
  }

  std::vector<std::complex<double>> internal;
  internal.reserve(shaped.size());
  double phase = 0.0;
  for (const auto value : shaped) {
    phase += value * std::numbers::pi / 16.0;
    internal.emplace_back(std::cos(phase), std::sin(phase));
  }

  auto samples = polyphase_resample(internal, 5'120'000U, sample_rate);
  const auto expected_count = static_cast<std::size_t>(std::llround(
      static_cast<double>(chips.size()) * static_cast<double>(sample_rate) /
      static_cast<double>(phy::chip_rate)));
  if (samples.size() > expected_count) samples.resize(expected_count);

  const auto duration = static_cast<double>(chips.size()) /
                        static_cast<double>(phy::chip_rate);
  return ComplexWaveform{WaveformModel::Efr32CustomOqpskMode2,
                         OqpskOrientation::EvenChipsOnI,
                         ChipPolarity::ZeroIsPositive,
                         true,
                         sample_rate,
                         symbols.size(),
                         chips.size(),
                         duration,
                         std::move(samples)};
}

}  // namespace bh61::dsp
