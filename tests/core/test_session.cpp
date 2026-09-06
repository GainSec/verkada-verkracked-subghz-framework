#include "bh61/core/session.hpp"
#include "bh61/core/crc16.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace {

using Block = std::array<std::uint8_t, 16>;

constexpr Block zero_block{};
constexpr Block reset_ciphertext{
    0x05, 0x6d, 0x33, 0x45, 0x38, 0x04, 0xc3, 0xc0,
    0x64, 0x0d, 0x83, 0xf6, 0x00, 0x9b, 0x6f, 0xc5};

}  // namespace

BH61_TEST("AES-128-CBC matches the recovered all-zero ResetReq vector") {
  const std::vector<std::uint8_t> padded_plaintext{
      0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const auto ciphertext =
      bh61::core::aes128_cbc_encrypt(padded_plaintext, zero_block, zero_block);
  BH61_REQUIRE(ciphertext == std::vector<std::uint8_t>(
                                  reset_ciphertext.begin(),
                                  reset_ciphertext.end()));
  BH61_REQUIRE(bh61::core::aes128_cbc_decrypt(ciphertext, zero_block,
                                              zero_block) == padded_plaintext);
}

BH61_TEST("AES-128-CBC matches the NIST SP 800-38A first block") {
  constexpr Block key{
      0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
      0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
  constexpr Block iv{
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  const std::vector<std::uint8_t> plaintext{
      0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
      0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
  const std::vector<std::uint8_t> expected{
      0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
      0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d};
  BH61_REQUIRE(bh61::core::aes128_cbc_encrypt(plaintext, key, iv) == expected);
  BH61_REQUIRE(bh61::core::aes128_cbc_decrypt(expected, key, iv) == plaintext);
}

BH61_TEST("protected VCMP open and seal reproduce ResetReq exactly") {
  const std::vector<std::uint8_t> exact{
      0x01, 0x01, 0x55, 0x30,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x06,
      0x05, 0x6d, 0x33, 0x45, 0x38, 0x04, 0xc3, 0xc0,
      0x64, 0x0d, 0x83, 0xf6, 0x00, 0x9b, 0x6f, 0xc5};
  const auto parsed = bh61::core::parse_vcmp(exact);
  BH61_REQUIRE(std::holds_alternative<bh61::core::VcmpFrame>(parsed));
  const auto opened = bh61::core::open_vcmp(
      std::get<bh61::core::VcmpFrame>(parsed), zero_block);
  BH61_REQUIRE(std::holds_alternative<bh61::core::OpenedVcmp>(opened));
  const auto& plaintext = std::get<bh61::core::OpenedVcmp>(opened);
  BH61_REQUIRE(plaintext.counter == 0);
  BH61_REQUIRE(plaintext.payload ==
               (std::vector<std::uint8_t>{0x14, 0x00, 0x00, 0x00}));

  const auto sealed = bh61::core::seal_vcmp(
      0x01, 0x01, plaintext.counter, plaintext.payload, zero_block, zero_block);
  BH61_REQUIRE(bh61::core::encode_vcmp(sealed) == exact);
}

BH61_TEST("VCMP clear IV and linear CRC permit a keyless first-block transform") {
  constexpr Block key{
      0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
      0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
  constexpr Block iv{
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  const std::vector<std::uint8_t> event_request{
      0x03, 0x7a, 0x0a, 0x00, 0x01, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const std::vector<std::uint8_t> target_request{
      0x1d, 0x7a, 0x01, 0x00, 0x01, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

  auto transformed =
      bh61::core::seal_vcmp(0x01, 0x01, 0x4242, event_request, key, iv);
  auto& encrypted = std::get<bh61::core::VcmpEncrypted>(transformed.body);

  std::vector<std::uint8_t> crc_delta(4U + 2U + event_request.size(), 0U);
  for (std::size_t index = 0; index < event_request.size(); ++index) {
    const auto delta = static_cast<std::uint8_t>(event_request[index] ^
                                                  target_request[index]);
    encrypted.iv[2U + index] ^= delta;
    crc_delta[4U + 2U + index] = delta;
  }
  transformed.received_crc ^= bh61::core::vcmp_crc16(crc_delta);

  const auto opened = bh61::core::open_vcmp(transformed, key);
  BH61_REQUIRE(std::holds_alternative<bh61::core::OpenedVcmp>(opened));
  const auto& plaintext = std::get<bh61::core::OpenedVcmp>(opened);
  BH61_REQUIRE(plaintext.counter == 0x4242);
  BH61_REQUIRE(plaintext.payload == target_request);
}

BH61_TEST("receive counter window uses 16-bit modular subtraction") {
  BH61_REQUIRE(bh61::core::counter_is_acceptable(0x1000, 0x1000));
  BH61_REQUIRE(bh61::core::counter_is_acceptable(0x1000, 0x1064));
  BH61_REQUIRE(!bh61::core::counter_is_acceptable(0x1000, 0x1065));
  BH61_REQUIRE(!bh61::core::counter_is_acceptable(0x1000, 0x0fff));
  BH61_REQUIRE(bh61::core::counter_is_acceptable(0xfffe, 0x0000));
  BH61_REQUIRE(bh61::core::next_counter(0xffff) == 0x0000);
}
