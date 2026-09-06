#pragma once

#include "bh61/radio/device.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bh61::radio {

struct TransmitRecord {
  std::uint64_t requested_time_ns{};
  std::size_t sample_count{};
  std::size_t first_collected_sample{};
};

class FileDevice final : public Device {
 public:
  FileDevice(std::span<const std::complex<float>> receive_samples,
             std::uint32_t sample_rate, std::uint64_t center_frequency_hz);

  auto capabilities() const -> DeviceCapabilities override;
  auto receive(std::size_t maximum_samples) -> SampleBlock override;
  void transmit(std::span<const std::complex<float>> samples,
                std::uint64_t requested_time_ns) override;

  auto eof() const -> bool;
  auto transmitted_samples() const
      -> const std::vector<std::complex<float>>&;
  auto transmit_records() const -> const std::vector<TransmitRecord>&;
  auto sample_rate() const -> std::uint32_t;
  auto center_frequency_hz() const -> std::uint64_t;

 private:
  std::vector<std::complex<float>> receive_samples_;
  std::vector<std::complex<float>> transmitted_samples_;
  std::vector<TransmitRecord> transmit_records_;
  std::size_t receive_offset_{};
  std::uint32_t sample_rate_{};
  std::uint64_t center_frequency_hz_{};
};

}  // namespace bh61::radio
