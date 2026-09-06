#include "bh61/core/messages.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

BH61_TEST("scan-info body is typed and byte-exact") {
  const std::array<std::uint8_t, 5> body{0x00, 0x15, 0x01, 0x00, 0x7a};
  const auto result = bh61::core::parse_scan_info(body);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ScanInfo>(result));
  const auto& scan = std::get<bh61::core::ScanInfo>(result);
  BH61_REQUIRE(scan.channel == 21);
  BH61_REQUIRE(scan.regulatory_code == 1);
  BH61_REQUIRE(scan.reserved == 0);
  BH61_REQUIRE(scan.sequence == 0x7a);
  BH61_REQUIRE(bh61::core::encode_scan_info(scan) ==
               std::vector<std::uint8_t>(body.begin(), body.end()));
}

BH61_TEST("counter-control subtypes have exact bodies") {
  const auto expected = bh61::core::parse_counter_control(
      std::array<std::uint8_t, 3>{0x01, 0x12, 0x34});
  BH61_REQUIRE(
      std::holds_alternative<bh61::core::CounterControl>(expected));
  const auto& control = std::get<bh61::core::CounterControl>(expected);
  BH61_REQUIRE(control.subtype ==
               bh61::core::CounterControlSubtype::ExpectedCounter);
  BH61_REQUIRE(control.value == 0x1234);
  BH61_REQUIRE(bh61::core::encode_counter_control(control) ==
               (std::vector<std::uint8_t>{0x01, 0x12, 0x34}));

  const auto missing = bh61::core::parse_counter_control(
      std::array<std::uint8_t, 1>{0x02});
  BH61_REQUIRE(std::holds_alternative<bh61::core::CounterControl>(missing));
}

BH61_TEST("RPC parser respects declared length and preserves trailing bytes") {
  const std::array<std::uint8_t, 7> bytes{
      0x13, 0x55, 0x02, 0x00, 0x00, 0x03, 0xaa};
  const auto result = bh61::core::parse_rpc(bytes);
  BH61_REQUIRE(std::holds_alternative<bh61::core::RpcMessage>(result));
  const auto& rpc = std::get<bh61::core::RpcMessage>(result);
  BH61_REQUIRE(rpc.message_id == 0x13);
  BH61_REQUIRE(rpc.transaction == 0x55);
  BH61_REQUIRE(rpc.declared_length == 2);
  BH61_REQUIRE(rpc.flags == 0x00);
  BH61_REQUIRE(rpc.payload == (std::vector<std::uint8_t>{0x00, 0x03}));
  BH61_REQUIRE(rpc.trailing == (std::vector<std::uint8_t>{0xaa}));
  BH61_REQUIRE(bh61::core::encode_rpc(rpc) ==
               std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

BH61_TEST("RPC parser rejects a declared length beyond the available body") {
  const std::array<std::uint8_t, 5> bytes{0x14, 0x00, 0x02, 0x00, 0x00};
  const auto result = bh61::core::parse_rpc(bytes);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(result).code ==
               bh61::core::ParseErrorCode::InvalidField);
}

BH61_TEST("version-4 join fixture parses every recovered field") {
  constexpr std::array<std::uint8_t, 26> body{
      0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x01, 0x41,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x01};
  const auto result = bh61::core::parse_join_request(body);
  BH61_REQUIRE(std::holds_alternative<bh61::core::JoinRequest>(result));
  const auto& join = std::get<bh61::core::JoinRequest>(result);
  BH61_REQUIRE(join.version == 4);
  BH61_REQUIRE(join.bsp == 0);
  BH61_REQUIRE(join.firmware_version == 0);
  BH61_REQUIRE(join.serial == (std::vector<std::uint8_t>{0x41}));
  BH61_REQUIRE(join.regulatory_code.has_value());
  BH61_REQUIRE(*join.regulatory_code == 1);
  BH61_REQUIRE(bh61::core::encode_join_request(join) ==
               std::vector<std::uint8_t>(body.begin(), body.end()));
}

BH61_TEST("version-6 join preserves recognized and unknown TLVs") {
  const std::vector<std::uint8_t> body{
      0x06, 0x02, 0x01, 0x02, 0x03, 0x04,
      0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x00, 0x02, 0x41, 0x42,
      0x01, 0x08, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
      0x03, 0x01, 0x01,
      0x7f, 0x02, 0xaa, 0xbb};
  const auto result = bh61::core::parse_join_request(body);
  BH61_REQUIRE(std::holds_alternative<bh61::core::JoinRequest>(result));
  const auto& join = std::get<bh61::core::JoinRequest>(result);
  BH61_REQUIRE(join.version == 6);
  BH61_REQUIRE(join.bsp == 2);
  BH61_REQUIRE(join.firmware_version == 0x01020304);
  BH61_REQUIRE(join.serial == (std::vector<std::uint8_t>{0x41, 0x42}));
  BH61_REQUIRE(join.nonce.has_value());
  BH61_REQUIRE(join.regulatory_code == 1);
  BH61_REQUIRE(join.tlvs.size() == 4);
  BH61_REQUIRE(join.tlvs.back().tag == 0x7f);
  BH61_REQUIRE(bh61::core::encode_join_request(join) == body);
}
