#include "bh61/dsp/modulator.hpp"
#include "bh61/evidence/writer.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 27> exact_type11_frame{
    0x1a, 0x41, 0xc8, 0x00, 0xff, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00,
    0x33, 0x8b, 0x00, 0x00, 0x01, 0x00, 0x00, 0x0e, 0x3a};

}  // namespace

BH61_TEST("analytic half-sine model produces exact scan-info timing") {
  const auto waveform = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  BH61_REQUIRE(waveform.model == bh61::dsp::WaveformModel::AnalyticHalfSine);
  BH61_REQUIRE(!waveform.hardware_exact);
  BH61_REQUIRE(waveform.total_symbols == 67U);
  BH61_REQUIRE(waveform.total_chips == 2'144U);
  BH61_REQUIRE(waveform.samples.size() == 13'400U);
  BH61_REQUIRE(std::abs(waveform.duration_seconds - 0.00335) < 1e-12);
  for (const auto sample : waveform.samples) {
    BH61_REQUIRE(std::isfinite(sample.real()));
    BH61_REQUIRE(std::isfinite(sample.imag()));
    BH61_REQUIRE(std::abs(sample) <= 1.0001F);
  }
}

BH61_TEST("analytic waveform generation is deterministic and orientation explicit") {
  const auto first = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  const auto second = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  BH61_REQUIRE(first.samples == second.samples);

  const auto swapped = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnQ,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  BH61_REQUIRE(swapped.samples.size() == first.samples.size());
  BH61_REQUIRE(swapped.samples != first.samples);

  const auto inverted = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsNegative);
  BH61_REQUIRE(inverted.samples.size() == first.samples.size());
  for (std::size_t index = 0; index < first.samples.size(); ++index) {
    BH61_REQUIRE(inverted.samples[index] == -first.samples[index]);
  }
}

BH61_TEST("canonical 4 MSPS fixture is immutable and numerically equivalent") {
  const auto waveform = bh61::dsp::modulate_oqpsk(
      exact_type11_frame, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  const auto fixture_directory =
      std::filesystem::path(BH61_SOURCE_DIR) / "fixtures" / "phy";
  const auto data_path =
      fixture_directory / "type11-f27-normal-destination-4msps.cf32";
  std::ifstream data(data_path, std::ios::binary);
  BH61_REQUIRE(data.good());
  std::vector<std::complex<float>> stored(waveform.samples.size());
  data.read(reinterpret_cast<char*>(stored.data()),
            static_cast<std::streamsize>(stored.size() *
                                         sizeof(std::complex<float>)));
  BH61_REQUIRE(data.gcount() ==
               static_cast<std::streamsize>(stored.size() *
                                            sizeof(std::complex<float>)));
  BH61_REQUIRE(data.peek() == std::ifstream::traits_type::eof());
  for (std::size_t index = 0; index < stored.size(); ++index) {
    // The analytical model uses the platform libm implementation. Preserve
    // the immutable fixture hash below, while allowing sub-float rounding
    // differences at half-sine zero crossings across standard libraries.
    BH61_REQUIRE(std::abs(stored[index].real() -
                          waveform.samples[index].real()) < 1e-6F);
    BH61_REQUIRE(std::abs(stored[index].imag() -
                          waveform.samples[index].imag()) < 1e-6F);
  }
  BH61_REQUIRE(
      bh61::evidence::sha256_file(data_path) ==
      "bcae4d289b994f4144d67319fcf11929cf9106c37e12fcb2c32000bd978d9caa");

  const auto metadata_path =
      fixture_directory / "type11-f27-normal-destination-4msps.json";
  std::ifstream metadata_input(metadata_path, std::ios::binary);
  BH61_REQUIRE(metadata_input.good());
  const std::string metadata(
      (std::istreambuf_iterator<char>(metadata_input)),
      std::istreambuf_iterator<char>());
  BH61_REQUIRE(metadata.find("\"sample_rate\": 4000000") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"sample_count\": 13400") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"hardware_exact\": false") !=
               std::string::npos);
}
