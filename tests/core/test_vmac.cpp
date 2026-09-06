#include "bh61/core/vmac.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace {

using Eui64 = std::array<std::uint8_t, 8>;

constexpr std::array<std::uint8_t, 24> exact_type11_psdu{
    0x41, 0xc8, 0x00, 0xff, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b,
    0x00, 0x33, 0x8b, 0x00, 0x00, 0x01, 0x00, 0x00};

}  // namespace

BH61_TEST("VMAC parser recovers the exact type-11 envelope") {
  const auto result = bh61::core::parse_vmac(exact_type11_psdu);
  BH61_REQUIRE(std::holds_alternative<bh61::core::VmacFrame>(result));
  const auto& frame = std::get<bh61::core::VmacFrame>(result);
  BH61_REQUIRE(frame.frame_control == 0xc841);
  BH61_REQUIRE(frame.sequence == 0x00);
  BH61_REQUIRE(frame.destination_pan == 0x01ff);
  BH61_REQUIRE(std::holds_alternative<std::uint16_t>(frame.destination));
  BH61_REQUIRE(std::get<std::uint16_t>(frame.destination) == 0x0001);
  BH61_REQUIRE(frame.source_eui ==
               (std::array<std::uint8_t, 8>{0, 0, 0, 0, 0, 0, 0, 0}));
  BH61_REQUIRE(frame.payload ==
               (std::vector<std::uint8_t>{0x0b, 0x00, 0x33, 0x8b,
                                          0x00, 0x00, 0x01, 0x00, 0x00}));
}

BH61_TEST("VMAC parser preserves and re-encodes opaque payload bytes") {
  const auto parsed = bh61::core::parse_vmac(exact_type11_psdu);
  BH61_REQUIRE(std::holds_alternative<bh61::core::VmacFrame>(parsed));
  const auto encoded =
      bh61::core::encode_vmac(std::get<bh61::core::VmacFrame>(parsed));
  BH61_REQUIRE(encoded == std::vector<std::uint8_t>(exact_type11_psdu.begin(),
                                                    exact_type11_psdu.end()));
}

BH61_TEST("VMAC parser rejects truncation and unsupported address forms") {
  const std::array<std::uint8_t, 7> truncated{0x41, 0xc8, 0x00, 0xff,
                                               0x01, 0x01, 0x00};
  const auto short_result = bh61::core::parse_vmac(truncated);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(short_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(short_result).code ==
               bh61::core::ParseErrorCode::Truncated);

  auto unsupported = exact_type11_psdu;
  unsupported[1] = 0x88;  // Source address mode 2 rather than required 3.
  const auto mode_result = bh61::core::parse_vmac(unsupported);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(mode_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(mode_result).code ==
               bh61::core::ParseErrorCode::UnsupportedAddressing);
}

BH61_TEST("VMAC parser accepts and preserves an extended destination") {
  constexpr std::array<std::uint8_t, 30> extended_psdu{
      0x41, 0xcc, 0x5a, 0xff, 0x01,
      0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
      0x0b, 0x00, 0x33, 0x8b, 0x00, 0x00, 0x01, 0x00, 0x00};
  const auto result = bh61::core::parse_vmac(extended_psdu);
  BH61_REQUIRE(std::holds_alternative<bh61::core::VmacFrame>(result));
  const auto& frame = std::get<bh61::core::VmacFrame>(result);
  constexpr Eui64 destination{
      0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
  constexpr Eui64 source{
      0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27};
  BH61_REQUIRE(std::holds_alternative<Eui64>(frame.destination));
  BH61_REQUIRE(std::get<Eui64>(frame.destination) == destination);
  BH61_REQUIRE(frame.source_eui == source);
  BH61_REQUIRE(bh61::core::destination_is_accepted(frame, destination));
  auto other = destination;
  other[7] ^= 0x01;
  BH61_REQUIRE(!bh61::core::destination_is_accepted(frame, other));
  BH61_REQUIRE(bh61::core::encode_vmac(frame) ==
               std::vector<std::uint8_t>(extended_psdu.begin(),
                                         extended_psdu.end()));
}

BH61_TEST("VMAC parser enforces firmware FCF masks and short destinations") {
  auto invalid_short = exact_type11_psdu;
  invalid_short[5] = 0x03;
  const auto destination_result = bh61::core::parse_vmac(invalid_short);
  BH61_REQUIRE(
      std::holds_alternative<bh61::core::ParseError>(destination_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(destination_result).code ==
               bh61::core::ParseErrorCode::InvalidDestination);

  auto security_enabled = exact_type11_psdu;
  security_enabled[0] |= 0x08;
  const auto fcf_result = bh61::core::parse_vmac(security_enabled);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(fcf_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(fcf_result).code ==
               bh61::core::ParseErrorCode::UnsupportedAddressing);
}
