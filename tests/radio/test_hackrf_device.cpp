#include "bh61/radio/hackrf_device.hpp"
#include "fake_hackrf_transport.hpp"
#include "test_harness.hpp"

#include <array>
#include <chrono>
#include <complex>
#include <cstdint>
#include <stdexcept>

namespace {

auto identity(std::string serial) -> bh61::radio::HackrfIdentity {
  return {std::move(serial), "HackRF Pro", "git-9039eb06 (API:1.09)",
          "r1.2"};
}

auto configuration(std::size_t queue_capacity = 4U)
    -> bh61::radio::HackrfConfiguration {
  bh61::radio::HackrfConfiguration result;
  result.serial = "target";
  result.queue_capacity_blocks = queue_capacity;
  return result;
}

}  // namespace

BH61_TEST("HackRF device selects identity and performs one clean lifecycle") {
  bh61::test::FakeHackrfTransport transport;
  transport.attached = {identity("other"), identity("target")};
  bh61::radio::HackrfDevice device(transport, configuration(),
                                   std::chrono::milliseconds(5));
  device.open();
  BH61_REQUIRE(device.identity().serial == "target");
  BH61_REQUIRE(device.realized_configuration().sample_rate == 4'000'000U);
  const auto capabilities = device.capabilities();
  BH61_REQUIRE(capabilities.rx);
#if defined(BH61_ENABLE_TX)
  BH61_REQUIRE(capabilities.tx);
#else
  BH61_REQUIRE(!capabilities.tx);
#endif
  BH61_REQUIRE(!capabilities.full_duplex);
  device.start_receive();
  device.stop();
  device.stop();
  device.close();
  device.close();
  BH61_REQUIRE(transport.open_calls == 1U);
  BH61_REQUIRE(transport.configure_calls == 1U);
  BH61_REQUIRE(transport.start_rx_calls == 1U);
  BH61_REQUIRE(transport.stop_rx_calls == 1U);
  BH61_REQUIRE(transport.close_calls == 1U);
}

BH61_TEST("HackRF callback blocks preserve order and absolute sample indices") {
  bh61::test::FakeHackrfTransport transport;
  transport.attached = {identity("target")};
  bh61::radio::HackrfDevice device(transport, configuration(),
                                   std::chrono::milliseconds(5));
  device.open();
  device.start_receive();
  constexpr std::array<std::int8_t, 4> first{-128, 0, 64, -64};
  constexpr std::array<std::int8_t, 4> second{127, 1, 0, 0};
  transport.emit(first, 100U);
  transport.emit(second, 200U);
  const auto first_block = device.receive(8U);
  const auto second_block = device.receive(8U);
  BH61_REQUIRE(first_block.status == bh61::radio::SampleBlockStatus::Data);
  BH61_REQUIRE(first_block.first_sample_index == 0U);
  BH61_REQUIRE(first_block.monotonic_time_ns == 100U);
  BH61_REQUIRE(first_block.samples[0] == std::complex<float>(-1.0F, 0.0F));
  BH61_REQUIRE(second_block.first_sample_index == 2U);
  BH61_REQUIRE(second_block.monotonic_time_ns == 200U);
  BH61_REQUIRE(second_block.samples[0] ==
               std::complex<float>(127.0F / 128.0F, 1.0F / 128.0F));
}

BH61_TEST("HackRF bounded queue reports overflow on the surviving block") {
  bh61::test::FakeHackrfTransport transport;
  transport.attached = {identity("target")};
  bh61::radio::HackrfDevice device(transport, configuration(1U),
                                   std::chrono::milliseconds(5));
  device.open();
  device.start_receive();
  constexpr std::array<std::int8_t, 4> first{1, 2, 3, 4};
  constexpr std::array<std::int8_t, 4> second{5, 6, 7, 8};
  transport.emit(first, 100U);
  transport.emit(second, 200U);
  const auto block = device.receive(8U);
  BH61_REQUIRE(block.first_sample_index == 2U);
  BH61_REQUIRE(block.discontinuity);
  BH61_REQUIRE(block.discontinuity_reason == "queue_overflow");
}

BH61_TEST("HackRF partial receives preserve sample index and time") {
  bh61::test::FakeHackrfTransport transport;
  transport.attached = {identity("target")};
  bh61::radio::HackrfDevice device(transport, configuration(),
                                   std::chrono::milliseconds(5));
  device.open();
  device.start_receive();
  constexpr std::array<std::int8_t, 6> samples{1, 2, 3, 4, 5, 6};
  transport.emit(samples, 1'000U);
  const auto first = device.receive(2U);
  const auto second = device.receive(2U);
  BH61_REQUIRE(first.samples.size() == 2U);
  BH61_REQUIRE(first.first_sample_index == 0U);
  BH61_REQUIRE(first.monotonic_time_ns == 1'000U);
  BH61_REQUIRE(second.samples.size() == 1U);
  BH61_REQUIRE(second.first_sample_index == 2U);
  BH61_REQUIRE(second.monotonic_time_ns == 1'500U);
}

BH61_TEST("HackRF receive distinguishes timeout cancellation and removal") {
  bh61::test::FakeHackrfTransport transport;
  transport.attached = {identity("target")};
  bh61::radio::HackrfDevice device(transport, configuration(),
                                   std::chrono::milliseconds(1));
  device.open();
  device.start_receive();
  const auto timeout = device.receive(8U);
  BH61_REQUIRE(timeout.status == bh61::radio::SampleBlockStatus::Timeout);
  transport.emit_status(bh61::radio::HackrfTransferStatus::DeviceRemoved,
                        "usb_removed");
  const auto removed = device.receive(8U);
  BH61_REQUIRE(removed.status ==
               bh61::radio::SampleBlockStatus::DeviceRemoved);
  BH61_REQUIRE(removed.discontinuity_reason == "usb_removed");

  bh61::test::FakeHackrfTransport second_transport;
  second_transport.attached = {identity("target")};
  bh61::radio::HackrfDevice second_device(second_transport, configuration(),
                                          std::chrono::milliseconds(1));
  second_device.open();
  second_device.start_receive();
  second_device.stop();
  const auto cancelled = second_device.receive(8U);
  BH61_REQUIRE(cancelled.status == bh61::radio::SampleBlockStatus::Cancelled);
}

BH61_TEST("HackRF configuration failure closes transport") {
  bh61::test::FakeHackrfTransport transport;
  transport.attached = {identity("target")};
  transport.fail_configuration = true;
  bool failed = false;
  try {
    bh61::radio::HackrfDevice device(transport, configuration(),
                                     std::chrono::milliseconds(1));
    device.open();
  } catch (const std::runtime_error&) {
    failed = true;
  }
  BH61_REQUIRE(failed);
  BH61_REQUIRE(transport.close_calls == 1U);

}

BH61_TEST("HackRF transmit obeys build capability and converts CF32") {
  bh61::test::FakeHackrfTransport transport;
  transport.attached = {identity("target")};
  bh61::radio::HackrfDevice device(transport, configuration(),
                                   std::chrono::milliseconds(1));
  device.open();
  constexpr std::array<std::complex<float>, 2> samples{
      std::complex<float>{-1.0F, 127.0F / 128.0F},
      std::complex<float>{0.5F, -0.5F}};
#if defined(BH61_ENABLE_TX)
  device.transmit(samples, 0U);
  BH61_REQUIRE(transport.transmit_calls == 1U);
  constexpr std::array<std::int8_t, 4> expected{-128, 127, 64, -64};
  BH61_REQUIRE(transport.transmitted_bytes.size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    BH61_REQUIRE(transport.transmitted_bytes[index] == expected[index]);
  }
#else
  bool rejected = false;
  try {
    device.transmit(samples, 0U);
  } catch (const std::logic_error&) {
    rejected = true;
  }
  BH61_REQUIRE(rejected);
  BH61_REQUIRE(transport.transmit_calls == 0U);
#endif
}

BH61_TEST("HackRF device repeats one hundred complete receive lifecycles") {
  bh61::test::FakeHackrfTransport transport;
  transport.attached = {identity("target")};
  for (std::size_t iteration = 0; iteration < 100U; ++iteration) {
    bh61::radio::HackrfDevice device(transport, configuration(),
                                     std::chrono::milliseconds(1));
    device.open();
    device.start_receive();
    device.stop();
    device.close();
  }
  BH61_REQUIRE(transport.open_calls == 100U);
  BH61_REQUIRE(transport.start_rx_calls == 100U);
  BH61_REQUIRE(transport.stop_rx_calls == 100U);
  BH61_REQUIRE(transport.close_calls == 100U);
}
