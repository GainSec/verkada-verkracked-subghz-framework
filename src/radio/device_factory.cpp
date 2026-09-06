#include "bh61/radio/device_factory.hpp"

#if defined(BH61_HAVE_HACKRF)
#include "bh61/radio/libhackrf_transport.hpp"
#endif

namespace bh61::radio {

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
                             false,
                             false, false, false, SampleFormat::SignedInt8,
                             2'000'000U, 20'000'000U}});
  }
#endif
  return devices;
}

}  // namespace bh61::radio
