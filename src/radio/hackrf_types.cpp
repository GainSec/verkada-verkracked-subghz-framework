#include "bh61/radio/hackrf_types.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace bh61::radio {
namespace {

constexpr std::uint32_t minimum_sample_rate = 2'000'000U;
constexpr std::uint32_t maximum_sample_rate = 20'000'000U;
constexpr std::uint64_t fcc_minimum_frequency_hz = 902'000'000U;
constexpr std::uint64_t fcc_maximum_frequency_hz = 928'000'000U;
constexpr std::uint32_t minimum_filter_hz = 1'750'000U;
constexpr std::uint32_t maximum_filter_hz = 28'000'000U;

auto encode_component(float value, std::size_t& clipped) -> std::int8_t {
  constexpr float minimum = -1.0F;
  constexpr float maximum = 127.0F / 128.0F;
  if (value < minimum || value > maximum) {
    ++clipped;
  }
  const auto bounded = std::clamp(value, minimum, maximum);
  return static_cast<std::int8_t>(std::lround(bounded * 128.0F));
}

}  // namespace

auto validate_hackrf_configuration(const HackrfConfiguration& configuration)
    -> std::vector<HackrfConfigurationError> {
  std::vector<HackrfConfigurationError> failures;
  if (configuration.serial.empty()) {
    failures.push_back(
        {HackrfConfigurationErrorCode::EmptySerial, "serial is empty"});
  }
  if (configuration.sample_rate < minimum_sample_rate ||
      configuration.sample_rate > maximum_sample_rate) {
    failures.push_back({HackrfConfigurationErrorCode::SampleRateOutOfRange,
                        "sample rate is outside 2-20 MS/s"});
  }
  if (configuration.center_frequency_hz < fcc_minimum_frequency_hz ||
      configuration.center_frequency_hz > fcc_maximum_frequency_hz) {
    failures.push_back(
        {HackrfConfigurationErrorCode::FrequencyOutsideFccProfile,
         "center frequency is outside the 902-928 MHz FCC profile"});
  }
  if (configuration.baseband_filter_hz < minimum_filter_hz ||
      configuration.baseband_filter_hz > maximum_filter_hz) {
    failures.push_back({HackrfConfigurationErrorCode::BasebandFilterOutOfRange,
                        "baseband filter is outside the supported range"});
  }
  if (configuration.lna_gain_db > 40U ||
      configuration.lna_gain_db % 8U != 0U) {
    failures.push_back({HackrfConfigurationErrorCode::LnaGainInvalid,
                        "LNA gain must be 0-40 dB in 8 dB steps"});
  }
  if (configuration.vga_gain_db > 62U ||
      configuration.vga_gain_db % 2U != 0U) {
    failures.push_back({HackrfConfigurationErrorCode::VgaGainInvalid,
                        "VGA gain must be 0-62 dB in 2 dB steps"});
  }
  if (configuration.tx_vga_gain_db > 47U) {
    failures.push_back({HackrfConfigurationErrorCode::TxVgaGainInvalid,
                        "TX VGA gain must be 0-47 dB"});
  }
  if (configuration.queue_capacity_blocks == 0U) {
    failures.push_back({HackrfConfigurationErrorCode::QueueSizeZero,
                        "queue capacity must be nonzero"});
  }
  return failures;
}

HackrfIqDecodeResult::HackrfIqDecodeResult(
    std::vector<std::complex<float>> samples)
    : samples_(std::move(samples)) {}

HackrfIqDecodeResult::HackrfIqDecodeResult(std::string error)
    : error_(std::move(error)) {}

auto HackrfIqDecodeResult::has_value() const noexcept -> bool {
  return error_.empty();
}

auto HackrfIqDecodeResult::operator->() const
    -> const std::vector<std::complex<float>>* {
  if (!has_value()) {
    throw std::logic_error("HackRF IQ conversion has no sample value");
  }
  return &samples_;
}

auto HackrfIqDecodeResult::operator*() const
    -> const std::vector<std::complex<float>>& {
  if (!has_value()) {
    throw std::logic_error("HackRF IQ conversion has no sample value");
  }
  return samples_;
}

auto HackrfIqDecodeResult::error() const -> const std::string& {
  if (has_value()) {
    throw std::logic_error("HackRF IQ conversion has no error");
  }
  return error_;
}

auto hackrf_iq_to_cf32(std::span<const std::int8_t> bytes)
    -> HackrfIqDecodeResult {
  if (bytes.size() % 2U != 0U) {
    return HackrfIqDecodeResult("odd_iq_byte_count");
  }
  std::vector<std::complex<float>> samples;
  samples.reserve(bytes.size() / 2U);
  for (std::size_t offset = 0; offset < bytes.size(); offset += 2U) {
    samples.emplace_back(static_cast<float>(bytes[offset]) / 128.0F,
                         static_cast<float>(bytes[offset + 1U]) / 128.0F);
  }
  return HackrfIqDecodeResult(std::move(samples));
}

auto cf32_to_hackrf_iq(std::span<const std::complex<float>> samples)
    -> HackrfIqEncodeResult {
  HackrfIqEncodeResult result;
  result.bytes.reserve(samples.size() * 2U);
  for (const auto sample : samples) {
    result.bytes.push_back(
        encode_component(sample.real(), result.clipped_components));
    result.bytes.push_back(
        encode_component(sample.imag(), result.clipped_components));
  }
  return result;
}

}  // namespace bh61::radio
