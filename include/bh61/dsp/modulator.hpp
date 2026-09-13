#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bh61::dsp {

enum class WaveformModel {
  AnalyticHalfSine,
  Efr32CustomOqpskMode2,
};

enum class OqpskOrientation {
  EvenChipsOnI,
  EvenChipsOnQ,
};

enum class ChipPolarity {
  ZeroIsPositive,
  ZeroIsNegative,
};

struct ComplexWaveform {
  WaveformModel model{WaveformModel::AnalyticHalfSine};
  OqpskOrientation orientation{OqpskOrientation::EvenChipsOnI};
  ChipPolarity polarity{ChipPolarity::ZeroIsPositive};
  bool hardware_exact{};
  std::uint32_t sample_rate{};
  std::size_t total_symbols{};
  std::size_t total_chips{};
  double duration_seconds{};
  std::vector<std::complex<float>> samples;
};

auto modulate_oqpsk(std::span<const std::uint8_t> frame_bytes,
                    std::uint32_t sample_rate,
                    OqpskOrientation orientation, ChipPolarity polarity)
    -> ComplexWaveform;

auto modulate_efr32_custom_oqpsk(
    std::span<const std::uint8_t> frame_bytes, std::uint32_t sample_rate)
    -> ComplexWaveform;

}  // namespace bh61::dsp
