#include "bh61/radio/file_device.hpp"
#include "test_harness.hpp"

#include <array>
#include <complex>
#include <cstdint>

BH61_TEST("file device replays timestamped blocks and declares capabilities") {
  constexpr std::array<std::complex<float>, 5> samples{
      std::complex<float>{1.0F, 0.0F}, std::complex<float>{0.5F, -0.5F},
      std::complex<float>{0.0F, 1.0F}, std::complex<float>{-1.0F, 0.0F},
      std::complex<float>{0.25F, 0.75F}};
  bh61::radio::FileDevice device(samples, 4'000'000U, 915'350'000U);
  const auto capabilities = device.capabilities();
  BH61_REQUIRE(capabilities.rx);
  BH61_REQUIRE(capabilities.tx);
  BH61_REQUIRE(!capabilities.full_duplex);
  BH61_REQUIRE(capabilities.hardware_timestamps);
  BH61_REQUIRE(capabilities.timed_tx);
  BH61_REQUIRE(capabilities.sample_format ==
               bh61::radio::SampleFormat::ComplexFloat32);
  BH61_REQUIRE(capabilities.minimum_sample_rate == 1U);
  BH61_REQUIRE(capabilities.maximum_sample_rate >= 4'000'000U);

  const auto first = device.receive(3U);
  BH61_REQUIRE(first.samples.size() == 3U);
  BH61_REQUIRE(first.first_sample_index == 0U);
  BH61_REQUIRE(first.monotonic_time_ns == 0U);
  BH61_REQUIRE(!first.discontinuity);
  const auto second = device.receive(3U);
  BH61_REQUIRE(second.samples.size() == 2U);
  BH61_REQUIRE(second.first_sample_index == 3U);
  BH61_REQUIRE(second.monotonic_time_ns == 750U);
  BH61_REQUIRE(device.eof());
}

BH61_TEST("file device collects timed transmit samples without hardware") {
  bh61::radio::FileDevice device({}, 4'000'000U, 915'350'000U);
  constexpr std::array<std::complex<float>, 2> burst{
      std::complex<float>{0.25F, -0.5F},
      std::complex<float>{-0.75F, 0.125F}};
  device.transmit(burst, 12'345U);
  BH61_REQUIRE(device.transmitted_samples().size() == burst.size());
  BH61_REQUIRE(device.transmitted_samples()[0] == burst[0]);
  BH61_REQUIRE(device.transmit_records().size() == 1U);
  BH61_REQUIRE(device.transmit_records()[0].requested_time_ns == 12'345U);
  BH61_REQUIRE(device.transmit_records()[0].sample_count == 2U);
}
