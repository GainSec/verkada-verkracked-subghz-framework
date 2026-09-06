#include "bh61/core/vcmp.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 9> plaintext_scan_info{
    0x0b, 0x00, 0x33, 0x8b, 0x00, 0x00, 0x01, 0x00, 0x00};

constexpr std::array<std::uint8_t, 37> encrypted_reset{
    0x01, 0x01, 0x55, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x06,
    0x05, 0x6d, 0x33, 0x45, 0x38, 0x04, 0xc3, 0xc0,
    0x64, 0x0d, 0x83, 0xf6, 0x00, 0x9b, 0x6f, 0xc5};

}  // namespace

BH61_TEST("VCMP parses and rebuilds plaintext scan-info") {
  const auto result = bh61::core::parse_vcmp(plaintext_scan_info);
  BH61_REQUIRE(std::holds_alternative<bh61::core::VcmpFrame>(result));
  const auto& frame = std::get<bh61::core::VcmpFrame>(result);
  BH61_REQUIRE(frame.type == 0x0b);
  BH61_REQUIRE(frame.flags == 0x00);
  BH61_REQUIRE(frame.received_crc == 0x338b);
  BH61_REQUIRE(frame.computed_crc == 0x338b);
  BH61_REQUIRE(std::holds_alternative<bh61::core::VcmpPlaintext>(frame.body));
  BH61_REQUIRE(std::get<bh61::core::VcmpPlaintext>(frame.body).bytes ==
               (std::vector<std::uint8_t>{0x00, 0x00, 0x01, 0x00, 0x00}));
  BH61_REQUIRE(bh61::core::encode_vcmp(frame) ==
               std::vector<std::uint8_t>(plaintext_scan_info.begin(),
                                         plaintext_scan_info.end()));
}

BH61_TEST("VCMP rejects a bad plaintext CRC") {
  auto corrupt = plaintext_scan_info;
  corrupt[8] ^= 0x01;
  const auto result = bh61::core::parse_vcmp(corrupt);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(result).code ==
               bh61::core::ParseErrorCode::BadChecksum);
}

BH61_TEST("VCMP structurally parses and rebuilds encrypted ResetReq") {
  const auto result = bh61::core::parse_vcmp(encrypted_reset);
  BH61_REQUIRE(std::holds_alternative<bh61::core::VcmpFrame>(result));
  const auto& frame = std::get<bh61::core::VcmpFrame>(result);
  BH61_REQUIRE(frame.type == 0x01);
  BH61_REQUIRE(frame.flags == 0x01);
  BH61_REQUIRE(frame.received_crc == 0x5530);
  BH61_REQUIRE(std::holds_alternative<bh61::core::VcmpEncrypted>(frame.body));
  const auto& encrypted = std::get<bh61::core::VcmpEncrypted>(frame.body);
  BH61_REQUIRE(encrypted.plaintext_length == 6);
  BH61_REQUIRE(encrypted.ciphertext.size() == 16);
  BH61_REQUIRE(bh61::core::encode_vcmp(frame) ==
               std::vector<std::uint8_t>(encrypted_reset.begin(),
                                         encrypted_reset.end()));
}

BH61_TEST("VCMP rejects inconsistent encrypted lengths") {
  std::vector<std::uint8_t> truncated(encrypted_reset.begin(),
                                      encrypted_reset.end() - 1);
  const auto truncated_result = bh61::core::parse_vcmp(truncated);
  BH61_REQUIRE(
      std::holds_alternative<bh61::core::ParseError>(truncated_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(truncated_result).code ==
               bh61::core::ParseErrorCode::InvalidEncoding);

  auto impossible = encrypted_reset;
  impossible[20] = 17;
  const auto impossible_result = bh61::core::parse_vcmp(impossible);
  BH61_REQUIRE(
      std::holds_alternative<bh61::core::ParseError>(impossible_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(impossible_result).code ==
               bh61::core::ParseErrorCode::InvalidEncoding);
}
