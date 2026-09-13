#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
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

struct CoherentSamplePair {
  SampleBlock channel_a;
  SampleBlock channel_b;
};

class Device {
 public:
  virtual ~Device() = default;
  virtual auto capabilities() const -> DeviceCapabilities = 0;
  virtual auto receive(std::size_t maximum_samples) -> SampleBlock = 0;
  virtual auto receive_pair(std::size_t maximum_samples)
      -> CoherentSamplePair {
    static_cast<void>(maximum_samples);
    throw std::logic_error("device does not support coherent two-channel RX");
  }
  virtual void start_receive_stream() {
    throw std::logic_error("device does not support continuous RX");
  }
  virtual void stop_receive_stream() {
    throw std::logic_error("device does not support continuous RX");
  }
  virtual auto current_time_ns() const -> std::uint64_t {
    throw std::logic_error("device does not expose its hardware clock");
  }
  virtual void transmit(std::span<const std::complex<float>> samples,
                        std::uint64_t requested_time_ns) = 0;
};

}  // namespace bh61::radio
