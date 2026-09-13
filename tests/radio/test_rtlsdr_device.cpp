#include "bh61/radio/rtlsdr_device.hpp"
#include "test_harness.hpp"

#include <array>

namespace {
class FakeRtlSdrTransport final : public bh61::radio::RtlSdrTransport {
 public:
  auto enumerate() -> std::vector<bh61::radio::RtlSdrIdentity> override {
    return {{"RTL-TEST", "RTL2838UHIDIR", 0U}};
  }
  void open(const bh61::radio::RtlSdrConfiguration&) override { opened = true; }
  auto receive(std::size_t) -> bh61::radio::SampleBlock override {
    return {{{0.0F, 0.0F}}, 0U, 1U, false, {},
            bh61::radio::SampleBlockStatus::Data};
  }
  void close() override { opened = false; }
  bool opened{};
};
}  // namespace

BH61_TEST("RTL-SDR is receive only in every build") {
  FakeRtlSdrTransport transport;
  bh61::radio::RtlSdrConfiguration configuration;
  configuration.serial = "RTL-TEST";
  bh61::radio::RtlSdrDevice device(transport, configuration);
  device.open();
  const auto capabilities = device.capabilities();
  BH61_REQUIRE(capabilities.rx);
  BH61_REQUIRE(!capabilities.tx);
  bool rejected = false;
  try {
    constexpr std::array<std::complex<float>, 1> samples{{{0.0F, 0.0F}}};
    device.transmit(samples, 0U);
  } catch (...) {
    rejected = true;
  }
  BH61_REQUIRE(rejected);
  BH61_REQUIRE(device.receive(1U).samples.size() == 1U);
}
