#include "bh61/radio/rtlsdr_device.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace bh61::radio {

RtlSdrDevice::RtlSdrDevice(RtlSdrTransport& transport,
                           RtlSdrConfiguration configuration)
    : transport_(transport), configuration_(std::move(configuration)) {}
RtlSdrDevice::~RtlSdrDevice() {
  try { close(); } catch (...) {}
}
void RtlSdrDevice::open() {
  if (opened_) return;
  if (configuration_.serial.empty()) throw std::invalid_argument("RTL-SDR serial is empty");
  if (configuration_.sample_rate < 225'001U || configuration_.sample_rate > 3'200'000U) {
    throw std::invalid_argument("RTL-SDR sample rate is outside its supported range");
  }
  if (configuration_.center_frequency_hz < 902'000'000U ||
      configuration_.center_frequency_hz > 928'000'000U) {
    throw std::invalid_argument("RTL-SDR frequency is outside 902-928 MHz");
  }
  const auto identities = transport_.enumerate();
  if (std::none_of(identities.begin(), identities.end(), [this](const auto& value) {
        return value.serial == configuration_.serial;
      })) {
    throw std::runtime_error("requested RTL-SDR is not attached");
  }
  transport_.open(configuration_);
  opened_ = true;
}
void RtlSdrDevice::close() {
  if (!opened_) return;
  transport_.close();
  opened_ = false;
}
auto RtlSdrDevice::capabilities() const -> DeviceCapabilities {
  return {true, false, false, false, false, SampleFormat::UnsignedInt8,
          225'001U, 3'200'000U};
}
auto RtlSdrDevice::receive(std::size_t maximum_samples) -> SampleBlock {
  if (!opened_) throw std::logic_error("RTL-SDR must be open before receive");
  if (maximum_samples == 0U) throw std::invalid_argument("RTL-SDR receive count must be nonzero");
  return transport_.receive(maximum_samples);
}
void RtlSdrDevice::transmit(std::span<const std::complex<float>>,
                            std::uint64_t) {
  throw std::logic_error("RTL-SDR is receive-only hardware");
}

}  // namespace bh61::radio
