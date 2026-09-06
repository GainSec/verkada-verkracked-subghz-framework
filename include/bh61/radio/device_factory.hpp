#pragma once

#include "bh61/radio/device.hpp"

#include <string>
#include <vector>

namespace bh61::radio {

struct DeviceDescriptor {
  std::string backend;
  std::string serial;
  std::string board_name;
  std::string firmware_version;
  std::string hardware_revision;
  DeviceCapabilities capabilities;
};

class DeviceFactory {
 public:
  virtual ~DeviceFactory() = default;
  virtual auto enumerate() -> std::vector<DeviceDescriptor> = 0;
};

class NativeDeviceFactory final : public DeviceFactory {
 public:
  auto enumerate() -> std::vector<DeviceDescriptor> override;
};

}  // namespace bh61::radio
