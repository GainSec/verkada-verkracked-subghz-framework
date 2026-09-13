#pragma once

#include "bh61/radio/device.hpp"
#include "bh61/radio/rtlsdr_transport.hpp"

namespace bh61::radio {

class RtlSdrDevice final : public Device {
 public:
  RtlSdrDevice(RtlSdrTransport& transport, RtlSdrConfiguration configuration);
  ~RtlSdrDevice() override;
  void open();
  void close();
  auto capabilities() const -> DeviceCapabilities override;
  auto receive(std::size_t maximum_samples) -> SampleBlock override;
  void transmit(std::span<const std::complex<float>> samples,
                std::uint64_t requested_time_ns) override;

 private:
  RtlSdrTransport& transport_;
  RtlSdrConfiguration configuration_;
  bool opened_{};
};

}  // namespace bh61::radio
