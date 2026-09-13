#pragma once

#include "bh61/radio/device.hpp"
#include "bh61/radio/hackrf_transport.hpp"
#include "bh61/radio/uhd_transport.hpp"
#include "bh61/radio/rtlsdr_transport.hpp"

#include <string>
#include <memory>
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

struct DeviceRequest {
  std::string backend;
  std::string serial;
  std::string fpga_path;
  std::uint32_t sample_rate{};
  std::uint64_t center_frequency_hz{};
  std::uint32_t tx_gain_db{};
  bool receive{true};
  bool coherent_receive{};
  bool transmit{};
};

class DeviceFactory {
 public:
  virtual ~DeviceFactory() = default;
  virtual auto enumerate() -> std::vector<DeviceDescriptor> = 0;
  virtual auto select(const DeviceRequest&) -> Device* { return nullptr; }
};

class NativeDeviceFactory final : public DeviceFactory {
 public:
  ~NativeDeviceFactory() override;
  auto enumerate() -> std::vector<DeviceDescriptor> override;
  auto select(const DeviceRequest& request) -> Device* override;

 private:
  std::unique_ptr<Device> selected_device_;
  std::unique_ptr<HackrfTransport> hackrf_transport_;
  std::unique_ptr<UhdTransport> uhd_transport_;
  std::unique_ptr<RtlSdrTransport> rtlsdr_transport_;
};

}  // namespace bh61::radio
