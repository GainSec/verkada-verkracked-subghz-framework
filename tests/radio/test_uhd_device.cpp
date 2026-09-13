#include "bh61/radio/uhd_device.hpp"
#include "test_harness.hpp"

#include <array>
#include <complex>

namespace {

class FakeUhdTransport final : public bh61::radio::UhdTransport {
 public:
  std::size_t open_calls{};
  std::size_t start_receive_calls{};
  std::size_t stop_receive_calls{};
  std::size_t receive_calls{};
  std::size_t transmit_calls{};
  std::uint64_t requested_time_ns{};
  std::vector<std::complex<float>> transmitted;
  bh61::radio::UhdConfiguration opened_configuration;

  auto enumerate() -> std::vector<bh61::radio::UhdIdentity> override {
    return {{"B210-TEST", "B210", "USRP B210"}};
  }
  void open(const bh61::radio::UhdConfiguration& configuration) override {
    ++open_calls;
    opened_configuration = configuration;
    BH61_REQUIRE(configuration.serial == "B210-TEST");
  }
  void start_receive_stream() override { ++start_receive_calls; }
  void stop_receive_stream() override { ++stop_receive_calls; }
  auto receive(std::size_t) -> bh61::radio::SampleBlock override {
    ++receive_calls;
    return {{{0.25F, -0.5F}}, 0U, 100U, false, {},
            bh61::radio::SampleBlockStatus::Data};
  }
  auto receive_pair(std::size_t) -> bh61::radio::CoherentSamplePair override {
    return {{{{1.0F, 0.0F}}, 7U, 123U, false, {},
             bh61::radio::SampleBlockStatus::Data},
            {{{0.0F, 1.0F}}, 7U, 123U, false, {},
             bh61::radio::SampleBlockStatus::Data}};
  }
  void transmit(std::span<const std::complex<float>> samples,
                std::uint64_t when) override {
    ++transmit_calls;
    requested_time_ns = when;
    transmitted.assign(samples.begin(), samples.end());
  }
  void close() override {}
};

}  // namespace

BH61_TEST("UHD device reports B210 full duplex and timed transmit") {
  FakeUhdTransport transport;
  bh61::radio::UhdConfiguration configuration;
  configuration.serial = "B210-TEST";
  bh61::radio::UhdDevice device(transport, configuration);
  device.open();
  const auto capabilities = device.capabilities();
  BH61_REQUIRE(capabilities.rx);
#if defined(BH61_ENABLE_TX)
  BH61_REQUIRE(capabilities.tx);
  BH61_REQUIRE(capabilities.timed_tx);
#else
  BH61_REQUIRE(!capabilities.tx);
#endif
  BH61_REQUIRE(capabilities.full_duplex);
}

BH61_TEST("UHD full-duplex sessions use RX2 while TX uses TX/RX") {
  FakeUhdTransport transport;
  bh61::radio::UhdConfiguration configuration;
  configuration.serial = "B210-TEST";
  configuration.enable_receive = true;
  configuration.enable_transmit = true;
  bh61::radio::UhdDevice device(transport, configuration);
  device.open();
  BH61_REQUIRE(transport.opened_configuration.rx_antenna == "RX2");
  BH61_REQUIRE(transport.opened_configuration.tx_antenna == "TX/RX");
}

BH61_TEST("UHD reactive sessions request a bounded deep RX frame queue") {
  FakeUhdTransport transport;
  bh61::radio::UhdConfiguration configuration;
  configuration.serial = "B210-TEST";
  bh61::radio::UhdDevice device(transport, configuration);
  device.open();
  BH61_REQUIRE(transport.opened_configuration.receive_frame_count == 1'024U);
}

BH61_TEST("UHD device preserves CF32 and requested transmit time") {
  FakeUhdTransport transport;
  bh61::radio::UhdConfiguration configuration;
  configuration.serial = "B210-TEST";
  bh61::radio::UhdDevice device(transport, configuration);
  device.open();
#if defined(BH61_ENABLE_TX)
  constexpr std::array<std::complex<float>, 2> samples{
      std::complex<float>{0.25F, -0.5F},
      std::complex<float>{-0.75F, 0.125F}};
  device.transmit(samples, 987654321U);
  BH61_REQUIRE(transport.transmit_calls == 1U);
  BH61_REQUIRE(transport.requested_time_ns == 987654321U);
  BH61_REQUIRE(transport.transmitted ==
               std::vector<std::complex<float>>(samples.begin(), samples.end()));
#endif
  const auto block = device.receive(32U);
  BH61_REQUIRE(block.samples.size() == 1U);
  BH61_REQUIRE(transport.receive_calls == 1U);
}

BH61_TEST("UHD device exposes an explicitly bounded continuous RX stream") {
  FakeUhdTransport transport;
  bh61::radio::UhdConfiguration configuration;
  configuration.serial = "B210-TEST";
  bh61::radio::UhdDevice device(transport, configuration);
  device.open();
  device.start_receive_stream();
  const auto block = device.receive(32U);
  device.stop_receive_stream();
  BH61_REQUIRE(block.status == bh61::radio::SampleBlockStatus::Data);
  BH61_REQUIRE(transport.start_receive_calls == 1U);
  BH61_REQUIRE(transport.receive_calls == 1U);
  BH61_REQUIRE(transport.stop_receive_calls == 1U);
}

BH61_TEST("UHD device returns timestamp-aligned coherent RX channels") {
  FakeUhdTransport transport;
  bh61::radio::UhdConfiguration configuration;
  configuration.serial = "B210-TEST";
  bh61::radio::UhdDevice device(transport, configuration);
  device.open();
  const auto pair = device.receive_pair(1U);
  BH61_REQUIRE(pair.channel_a.samples.size() == 1U);
  BH61_REQUIRE(pair.channel_b.samples.size() == 1U);
  BH61_REQUIRE(pair.channel_a.first_sample_index ==
               pair.channel_b.first_sample_index);
  BH61_REQUIRE(pair.channel_a.monotonic_time_ns ==
               pair.channel_b.monotonic_time_ns);
}
