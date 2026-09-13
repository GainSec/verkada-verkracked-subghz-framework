#pragma once

#include "bh61/radio/device.hpp"
#include "bh61/radio/uhd_transport.hpp"

namespace bh61::radio {

class UhdDevice final : public Device {
 public:
  UhdDevice(UhdTransport& transport, UhdConfiguration configuration);
  ~UhdDevice() override;

  void open();
  void close();
  auto identity() const -> const UhdIdentity&;
  auto capabilities() const -> DeviceCapabilities override;
  void start_receive_stream() override;
  void stop_receive_stream() override;
  auto receive(std::size_t maximum_samples) -> SampleBlock override;
  auto receive_pair(std::size_t maximum_samples) -> CoherentSamplePair override;
  auto current_time_ns() const -> std::uint64_t override;
  void transmit(std::span<const std::complex<float>> samples,
                std::uint64_t requested_time_ns) override;

 private:
  UhdTransport& transport_;
  UhdConfiguration configuration_;
  UhdIdentity identity_;
  bool opened_{};
};

}  // namespace bh61::radio
