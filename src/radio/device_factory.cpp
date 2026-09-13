#include "bh61/radio/device_factory.hpp"
#include "bh61/radio/hackrf_transport.hpp"

#if defined(BH61_HAVE_HACKRF)
#include "bh61/radio/hackrf_device.hpp"
#include "bh61/radio/libhackrf_transport.hpp"
#endif
#if defined(BH61_HAVE_RTLSDR)
#include "bh61/radio/librtlsdr_transport.hpp"
#include "bh61/radio/rtlsdr_device.hpp"
#endif
#if defined(BH61_HAVE_UHD)
#include "bh61/radio/libuhd_transport.hpp"
#include "bh61/radio/uhd_device.hpp"
#endif

#include <chrono>
#include <stdexcept>

namespace bh61::radio {

NativeDeviceFactory::~NativeDeviceFactory() {
  // Devices hold non-owning references to their transports and close through
  // those references during destruction. Destroy the selected device first.
  selected_device_.reset();
}

auto NativeDeviceFactory::enumerate() -> std::vector<DeviceDescriptor> {
  std::vector<DeviceDescriptor> devices;
#if defined(BH61_HAVE_HACKRF)
  LibhackrfTransport transport;
  for (auto& identity : transport.enumerate()) {
    devices.push_back(
        DeviceDescriptor{"hackrf",
                         std::move(identity.serial),
                         std::move(identity.board_name),
                         std::move(identity.firmware_version),
                         std::move(identity.hardware_revision),
                         DeviceCapabilities{
                             true,
#if defined(BH61_ENABLE_TX)
                             true,
#else
                             false,
#endif
                             false, false, false, SampleFormat::SignedInt8,
                             2'000'000U, 20'000'000U}});
  }
#endif
#if defined(BH61_HAVE_UHD)
  LibuhdTransport uhd;
  for (auto& identity : uhd.enumerate()) {
    devices.push_back(DeviceDescriptor{
        "uhd", std::move(identity.serial), std::move(identity.product), "UHD",
        std::move(identity.name),
        DeviceCapabilities{true,
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
                           SampleFormat::ComplexFloat32, 1'000'000U,
                           61'440'000U}});
  }
#endif
#if defined(BH61_HAVE_RTLSDR)
  LibrtlSdrTransport rtl;
  for (auto& identity : rtl.enumerate()) {
    devices.push_back(DeviceDescriptor{
        "rtlsdr", std::move(identity.serial), std::move(identity.name),
        "librtlsdr", "receive-only",
        DeviceCapabilities{true, false, false, false, false,
                           SampleFormat::UnsignedInt8, 225'001U, 3'200'000U}});
  }
#endif
  return devices;
}

auto NativeDeviceFactory::select(const DeviceRequest& request) -> Device* {
  selected_device_.reset();
  hackrf_transport_.reset();
  uhd_transport_.reset();
  rtlsdr_transport_.reset();
#if defined(BH61_HAVE_HACKRF)
  if (request.backend == "hackrf") {
    if (request.serial.empty()) {
      throw std::invalid_argument("HackRF selection requires --serial");
    }
    auto transport = std::make_unique<LibhackrfTransport>();
    HackrfConfiguration configuration;
    configuration.serial = request.serial;
    configuration.sample_rate = request.sample_rate;
    configuration.center_frequency_hz = request.center_frequency_hz;
    configuration.tx_vga_gain_db = request.tx_gain_db;
    auto device = std::make_unique<HackrfDevice>(
        *transport, configuration, std::chrono::milliseconds(1000));
    device->open();
    hackrf_transport_ = std::move(transport);
    selected_device_ = std::move(device);
    return selected_device_.get();
  }
#endif
#if defined(BH61_HAVE_RTLSDR)
  if (request.backend == "rtlsdr") {
    if (request.serial.empty()) {
      throw std::invalid_argument("RTL-SDR selection requires --serial");
    }
    auto transport = std::make_unique<LibrtlSdrTransport>();
    RtlSdrConfiguration configuration;
    configuration.serial = request.serial;
    configuration.sample_rate = request.sample_rate;
    configuration.center_frequency_hz =
        static_cast<std::uint32_t>(request.center_frequency_hz);
    auto device = std::make_unique<RtlSdrDevice>(*transport, configuration);
    device->open();
    rtlsdr_transport_ = std::move(transport);
    selected_device_ = std::move(device);
    return selected_device_.get();
  }
#endif
#if defined(BH61_HAVE_UHD)
  if (request.backend == "uhd" || request.backend == "b210") {
    if (request.serial.empty()) {
      throw std::invalid_argument("UHD selection requires --serial");
    }
    auto transport = std::make_unique<LibuhdTransport>();
    UhdConfiguration configuration;
    configuration.serial = request.serial;
    configuration.fpga_path = request.fpga_path;
    configuration.sample_rate = static_cast<double>(request.sample_rate);
    configuration.center_frequency_hz =
        static_cast<double>(request.center_frequency_hz);
    configuration.tx_gain_db = static_cast<double>(request.tx_gain_db);
    configuration.enable_receive = request.receive;
    configuration.enable_coherent_receive = request.coherent_receive;
    configuration.enable_transmit = request.transmit;
    auto device = std::make_unique<UhdDevice>(*transport, configuration);
    device->open();
    uhd_transport_ = std::move(transport);
    selected_device_ = std::move(device);
    return selected_device_.get();
  }
#endif
  throw std::invalid_argument("requested native backend is unavailable: " +
                              request.backend);
}

}  // namespace bh61::radio
