#include "bh61/radio/uhd_device.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace bh61::radio {

UhdDevice::UhdDevice(UhdTransport& transport, UhdConfiguration configuration)
    : transport_(transport), configuration_(std::move(configuration)) {}

UhdDevice::~UhdDevice() {
  try {
    close();
  } catch (...) {
  }
}

void UhdDevice::open() {
  if (opened_) return;
  if (configuration_.serial.empty()) {
    throw std::invalid_argument("UHD serial is empty");
  }
  if (configuration_.sample_rate < 1'000'000.0 ||
      configuration_.sample_rate > 61'440'000.0) {
    throw std::invalid_argument("UHD sample rate is outside 1-61.44 MS/s");
  }
  if (configuration_.center_frequency_hz < 902'000'000.0 ||
      configuration_.center_frequency_hz > 928'000'000.0) {
    throw std::invalid_argument("UHD frequency is outside 902-928 MHz");
  }
  if (configuration_.receive_frame_count < 16U ||
      configuration_.receive_frame_count > 4'096U) {
    throw std::invalid_argument("UHD receive frame count is outside 16-4096");
  }
  const auto identities = transport_.enumerate();
  const auto selected = std::find_if(
      identities.begin(), identities.end(), [this](const auto& identity) {
        return identity.serial == configuration_.serial;
      });
  if (selected == identities.end()) {
    throw std::runtime_error("requested UHD device is not attached");
  }
  if (configuration_.enable_transmit && configuration_.tx_antenna.empty()) {
    configuration_.tx_antenna = "TX/RX";
  }
  if (configuration_.enable_receive && configuration_.enable_transmit &&
      configuration_.rx_antenna.empty()) {
    configuration_.rx_antenna = "RX2";
  }
  transport_.open(configuration_);
  identity_ = *selected;
  opened_ = true;
}

void UhdDevice::close() {
  if (!opened_) return;
  transport_.close();
  opened_ = false;
}

auto UhdDevice::identity() const -> const UhdIdentity& {
  if (!opened_) throw std::logic_error("UHD identity requested before open");
  return identity_;
}

auto UhdDevice::capabilities() const -> DeviceCapabilities {
  return {true,
#if defined(BH61_ENABLE_TX)
          true,
#else
          false,
#endif
          true, true,
#if defined(BH61_ENABLE_TX)
          true,
#else
          false,
#endif
          SampleFormat::ComplexFloat32, 1'000'000U, 61'440'000U};
}

void UhdDevice::start_receive_stream() {
  if (!opened_) throw std::logic_error("UHD device must be open before receive");
  transport_.start_receive_stream();
}

void UhdDevice::stop_receive_stream() {
  if (!opened_) throw std::logic_error("UHD device must be open before receive");
  transport_.stop_receive_stream();
}

auto UhdDevice::receive(std::size_t maximum_samples) -> SampleBlock {
  if (!opened_) throw std::logic_error("UHD device must be open before receive");
  if (maximum_samples == 0U) {
    throw std::invalid_argument("UHD receive sample count must be nonzero");
  }
  return transport_.receive(maximum_samples);
}

auto UhdDevice::receive_pair(std::size_t maximum_samples)
    -> CoherentSamplePair {
  if (!opened_) {
    throw std::logic_error("UHD device must be open before receive");
  }
  if (maximum_samples == 0U) {
    throw std::invalid_argument("UHD receive sample count must be nonzero");
  }
  return transport_.receive_pair(maximum_samples);
}

auto UhdDevice::current_time_ns() const -> std::uint64_t {
  if (!opened_) throw std::logic_error("UHD device is not open");
  return transport_.current_time_ns();
}

void UhdDevice::transmit(std::span<const std::complex<float>> samples,
                         std::uint64_t requested_time_ns) {
#if defined(BH61_ENABLE_TX)
  if (!opened_) throw std::logic_error("UHD device must be open before transmit");
  if (samples.empty()) {
    throw std::invalid_argument("UHD transmit sample count must be nonzero");
  }
  transport_.transmit(samples, requested_time_ns);
#else
  static_cast<void>(samples);
  static_cast<void>(requested_time_ns);
  throw std::logic_error("UHD transmit is unavailable in this build");
#endif
}

}  // namespace bh61::radio
