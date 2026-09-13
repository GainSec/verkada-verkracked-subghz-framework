#include "bh61/radio/librtlsdr_transport.hpp"

#include <rtl-sdr.h>

#include <chrono>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace bh61::radio {
struct LibrtlSdrTransport::Implementation {
  rtlsdr_dev_t* device{};
  std::uint64_t sample_index{};
};
LibrtlSdrTransport::LibrtlSdrTransport()
    : implementation_(std::make_unique<Implementation>()) {}
LibrtlSdrTransport::~LibrtlSdrTransport() { close(); }
auto LibrtlSdrTransport::enumerate() -> std::vector<RtlSdrIdentity> {
  std::vector<RtlSdrIdentity> result;
  for (std::uint32_t index = 0; index < rtlsdr_get_device_count(); ++index) {
    std::array<char, 256> manufacturer{}, product{}, serial{};
    if (rtlsdr_get_device_usb_strings(index, manufacturer.data(), product.data(),
                                     serial.data()) == 0) {
      result.push_back({serial.data(), rtlsdr_get_device_name(index), index});
    }
  }
  return result;
}
void LibrtlSdrTransport::open(const RtlSdrConfiguration& configuration) {
  if (implementation_->device) throw std::logic_error("an RTL-SDR is already open");
  const auto identities = enumerate();
  const auto found = std::find_if(identities.begin(), identities.end(),
      [&configuration](const auto& identity) { return identity.serial == configuration.serial; });
  if (found == identities.end()) throw std::runtime_error("RTL-SDR serial not found");
  if (rtlsdr_open(&implementation_->device, found->index) != 0 ||
      rtlsdr_set_sample_rate(implementation_->device, configuration.sample_rate) != 0 ||
      rtlsdr_set_center_freq(implementation_->device, configuration.center_frequency_hz) != 0 ||
      rtlsdr_set_tuner_gain_mode(implementation_->device, 1) != 0 ||
      rtlsdr_set_tuner_gain(implementation_->device, configuration.tuner_gain_tenths_db) != 0 ||
      rtlsdr_reset_buffer(implementation_->device) != 0) {
    close();
    throw std::runtime_error("RTL-SDR configuration failed");
  }
  implementation_->sample_index = 0U;
}
auto LibrtlSdrTransport::receive(std::size_t maximum_samples) -> SampleBlock {
  if (!implementation_->device) throw std::logic_error("RTL-SDR RX requires an open device");
  std::vector<std::uint8_t> bytes(maximum_samples * 2U);
  int read{};
  if (rtlsdr_read_sync(implementation_->device, bytes.data(),
                       static_cast<int>(bytes.size()), &read) != 0) {
    return {{}, implementation_->sample_index, 0U, true, "rtlsdr_read_sync",
            SampleBlockStatus::Error};
  }
  std::vector<std::complex<float>> samples;
  samples.reserve(static_cast<std::size_t>(read) / 2U);
  for (int offset = 0; offset + 1 < read; offset += 2) {
    samples.emplace_back((static_cast<float>(bytes[offset]) - 127.5F) / 127.5F,
                         (static_cast<float>(bytes[offset + 1]) - 127.5F) / 127.5F);
  }
  const auto first = implementation_->sample_index;
  implementation_->sample_index += samples.size();
  const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  return {std::move(samples), first, now, false, {}, SampleBlockStatus::Data};
}
void LibrtlSdrTransport::close() {
  if (!implementation_->device) return;
  rtlsdr_close(implementation_->device);
  implementation_->device = nullptr;
}
}  // namespace bh61::radio
