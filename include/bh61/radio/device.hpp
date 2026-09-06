#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bh61::radio {

enum class SampleFormat {
  ComplexFloat32,
  SignedInt8,
  UnsignedInt8,
};

struct DeviceCapabilities {
  bool rx{};
  bool tx{};
  bool full_duplex{};
  bool hardware_timestamps{};
  bool timed_tx{};
  SampleFormat sample_format{SampleFormat::ComplexFloat32};
  std::uint32_t minimum_sample_rate{};
  std::uint32_t maximum_sample_rate{};
};

enum class SampleBlockStatus {
  Data,
  Timeout,
  EndOfStream,
  Cancelled,
  DeviceRemoved,
  Error,
};

struct SampleBlock {
  std::vector<std::complex<float>> samples;
  std::uint64_t first_sample_index{};
  std::uint64_t monotonic_time_ns{};
  bool discontinuity{};
  std::string discontinuity_reason;
  SampleBlockStatus status{SampleBlockStatus::Data};
};

class Device {
 public:
  virtual ~Device() = default;
  virtual auto capabilities() const -> DeviceCapabilities = 0;
  virtual auto receive(std::size_t maximum_samples) -> SampleBlock = 0;
  virtual void transmit(std::span<const std::complex<float>> samples,
                        std::uint64_t requested_time_ns) = 0;
};

}  // namespace bh61::radio
