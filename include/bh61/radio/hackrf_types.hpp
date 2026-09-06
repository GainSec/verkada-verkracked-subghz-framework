#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bh61::radio {

struct HackrfConfiguration {
  std::string serial;
  std::uint32_t sample_rate{4'000'000U};
  std::uint64_t center_frequency_hz{915'350'000U};
  std::uint32_t baseband_filter_hz{3'500'000U};
  std::uint32_t lna_gain_db{16U};
  std::uint32_t vga_gain_db{20U};
  std::uint32_t tx_vga_gain_db{};
  bool amplifier_enabled{};
  bool antenna_power_enabled{};
  std::size_t queue_capacity_blocks{32U};
};

enum class HackrfConfigurationErrorCode {
  EmptySerial,
  SampleRateOutOfRange,
  FrequencyOutsideFccProfile,
  BasebandFilterOutOfRange,
  LnaGainInvalid,
  VgaGainInvalid,
  TxVgaGainInvalid,
  QueueSizeZero,
};

struct HackrfConfigurationError {
  HackrfConfigurationErrorCode code;
  std::string message;
};

auto validate_hackrf_configuration(const HackrfConfiguration& configuration)
    -> std::vector<HackrfConfigurationError>;

class HackrfIqDecodeResult {
 public:
  explicit HackrfIqDecodeResult(std::vector<std::complex<float>> samples);
  explicit HackrfIqDecodeResult(std::string error);

  auto has_value() const noexcept -> bool;
  auto operator->() const -> const std::vector<std::complex<float>>*;
  auto operator*() const -> const std::vector<std::complex<float>>&;
  auto error() const -> const std::string&;

 private:
  std::vector<std::complex<float>> samples_;
  std::string error_;
};

struct HackrfIqEncodeResult {
  std::vector<std::int8_t> bytes;
  std::size_t clipped_components{};
};

auto hackrf_iq_to_cf32(std::span<const std::int8_t> bytes)
    -> HackrfIqDecodeResult;
auto cf32_to_hackrf_iq(std::span<const std::complex<float>> samples)
    -> HackrfIqEncodeResult;

}  // namespace bh61::radio
