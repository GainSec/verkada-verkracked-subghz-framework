#include "bh61/core/p256.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace {

template <std::size_t N>
auto hex_array(std::string_view text) -> std::array<std::uint8_t, N> {
  if (text.size() != N * 2U) {
    throw std::invalid_argument("wrong hexadecimal fixture length");
  }
  auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
    throw std::invalid_argument("invalid hexadecimal fixture");
  };
  std::array<std::uint8_t, N> result{};
  for (std::size_t index = 0; index < N; ++index) {
    result[index] = static_cast<std::uint8_t>((nibble(text[index * 2U]) << 4U) |
                                              nibble(text[index * 2U + 1U]));
  }
  return result;
}

template <typename Function>
auto throws_invalid(Function&& function) -> bool {
  try {
    function();
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

}  // namespace

BH61_TEST("P-256 published generator vector derives symmetric shared X") {
  const auto private_one = hex_array<32>(
      "0000000000000000000000000000000000000000000000000000000000000001");
  const auto private_two = hex_array<32>(
      "0000000000000000000000000000000000000000000000000000000000000002");
  const auto public_one = hex_array<64>(
      "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
      "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
  const auto public_two = hex_array<64>(
      "7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978"
      "07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1");
  const auto shared_x = hex_array<32>(
      "7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978");

  BH61_REQUIRE(bh61::core::p256_public_from_private(private_one) == public_one);
  BH61_REQUIRE(bh61::core::p256_public_from_private(private_two) == public_two);
  BH61_REQUIRE(bh61::core::p256_shared_x(private_one, public_two) == shared_x);
  BH61_REQUIRE(bh61::core::p256_shared_x(private_two, public_one) == shared_x);
  BH61_REQUIRE(bh61::core::derive_peer_key(shared_x) ==
               hex_array<16>("23775201799b2234a18e8071e409cec8"));
}

BH61_TEST("P-256 rejects invalid private scalars and public points") {
  const std::array<std::uint8_t, 32> zero{};
  const auto order = hex_array<32>(
      "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
  const auto one = hex_array<32>(
      "0000000000000000000000000000000000000000000000000000000000000001");
  std::array<std::uint8_t, 64> off_curve{};
  off_curve[63] = 1;
  BH61_REQUIRE(throws_invalid([&] { (void)bh61::core::p256_public_from_private(zero); }));
  BH61_REQUIRE(throws_invalid([&] { (void)bh61::core::p256_public_from_private(order); }));
  BH61_REQUIRE(throws_invalid([&] { (void)bh61::core::p256_shared_x(one, off_curve); }));
}

BH61_TEST("P-256 generated keys are valid and not fixed") {
  const auto first = bh61::core::generate_p256_keypair();
  const auto second = bh61::core::generate_p256_keypair();
  BH61_REQUIRE(first.private_scalar != second.private_scalar);
  BH61_REQUIRE(first.public_xy != second.public_xy);
  BH61_REQUIRE(bh61::core::p256_public_from_private(first.private_scalar) == first.public_xy);
}
