#include "bh61/radio/hackrf_types.hpp"
#include "test_harness.hpp"

#include <array>
#include <complex>
#include <cstdint>

BH61_TEST("HackRF configuration accepts the recovered FCC capture profile") {
  bh61::radio::HackrfConfiguration configuration;
  configuration.serial = "00000000000000000000000000000001";
  configuration.sample_rate = 4'000'000U;
  configuration.center_frequency_hz = 915'350'000U;
  configuration.baseband_filter_hz = 3'500'000U;
  configuration.lna_gain_db = 16U;
  configuration.vga_gain_db = 20U;
  configuration.tx_vga_gain_db = 0U;
  configuration.queue_capacity_blocks = 32U;
  BH61_REQUIRE(bh61::radio::validate_hackrf_configuration(configuration).empty());
}

BH61_TEST("HackRF configuration reports stable errors for every invalid field") {
  bh61::radio::HackrfConfiguration configuration;
  configuration.sample_rate = 1U;
  configuration.center_frequency_hz = 900'000'000U;
  configuration.baseband_filter_hz = 1U;
  configuration.lna_gain_db = 7U;
  configuration.vga_gain_db = 3U;
  configuration.tx_vga_gain_db = 48U;
  configuration.queue_capacity_blocks = 0U;
  const auto failures =
      bh61::radio::validate_hackrf_configuration(configuration);
  BH61_REQUIRE(failures.size() == 8U);
  BH61_REQUIRE(failures[0].code ==
               bh61::radio::HackrfConfigurationErrorCode::EmptySerial);
  BH61_REQUIRE(failures[1].code ==
               bh61::radio::HackrfConfigurationErrorCode::SampleRateOutOfRange);
  BH61_REQUIRE(failures[2].code == bh61::radio::
                                       HackrfConfigurationErrorCode::
                                           FrequencyOutsideFccProfile);
  BH61_REQUIRE(failures[3].code == bh61::radio::
                                       HackrfConfigurationErrorCode::
                                           BasebandFilterOutOfRange);
  BH61_REQUIRE(failures[4].code ==
               bh61::radio::HackrfConfigurationErrorCode::LnaGainInvalid);
  BH61_REQUIRE(failures[5].code ==
               bh61::radio::HackrfConfigurationErrorCode::VgaGainInvalid);
  BH61_REQUIRE(failures[6].code ==
               bh61::radio::HackrfConfigurationErrorCode::TxVgaGainInvalid);
  BH61_REQUIRE(failures[7].code ==
               bh61::radio::HackrfConfigurationErrorCode::QueueSizeZero);
}

BH61_TEST("HackRF signed byte IQ converts exactly to normalized complex float") {
  constexpr std::array<std::int8_t, 8> raw{
      -128, 127, -64, 64, 0, 0, 127, -128};
  const auto result = bh61::radio::hackrf_iq_to_cf32(raw);
  BH61_REQUIRE(result.has_value());
  BH61_REQUIRE(result->size() == 4U);
  BH61_REQUIRE((*result)[0] == std::complex<float>(-1.0F, 127.0F / 128.0F));
  BH61_REQUIRE((*result)[1] == std::complex<float>(-0.5F, 0.5F));
  BH61_REQUIRE((*result)[2] == std::complex<float>(0.0F, 0.0F));
  BH61_REQUIRE((*result)[3] == std::complex<float>(127.0F / 128.0F, -1.0F));
}

BH61_TEST("HackRF receive conversion rejects an unpaired IQ component") {
  constexpr std::array<std::int8_t, 3> raw{1, 2, 3};
  const auto result = bh61::radio::hackrf_iq_to_cf32(raw);
  BH61_REQUIRE(!result.has_value());
  BH61_REQUIRE(result.error() == "odd_iq_byte_count");
}

BH61_TEST("HackRF transmit conversion rounds deterministically and counts clips") {
  constexpr std::array<std::complex<float>, 4> samples{
      std::complex<float>{-1.0F, 127.0F / 128.0F},
      std::complex<float>{-0.5F, 0.5F},
      std::complex<float>{-1.5F, 1.5F},
      std::complex<float>{0.00390625F, -0.00390625F}};
  const auto result = bh61::radio::cf32_to_hackrf_iq(samples);
  constexpr std::array<std::int8_t, 8> expected{
      -128, 127, -64, 64, -128, 127, 1, -1};
  BH61_REQUIRE(result.bytes.size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    BH61_REQUIRE(result.bytes[index] == expected[index]);
  }
  BH61_REQUIRE(result.clipped_components == 2U);
}
