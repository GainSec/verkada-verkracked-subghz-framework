#include "bh61/core/messages.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

namespace {

template <typename Result>
auto is_parse_error(const Result& result) -> bool {
  return std::holds_alternative<bh61::core::ParseError>(result);
}

}  // namespace

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

BH61_TEST("join response formats 3 4 and 5 are byte exact") {
  bh61::core::JoinResponse response{};
  for (std::size_t index = 0; index < response.local_public_xy.size(); ++index) {
    response.local_public_xy[index] = static_cast<std::uint8_t>(index);
  }
  response.local_counter = 0x1234;
  response.remote_counter = 0x5678;

  response.format = 3;
  const auto format3 = bh61::core::encode_join_response(response);
  BH61_REQUIRE(format3.size() == 85U);
  const auto parsed3 = bh61::core::parse_join_response(format3);
  BH61_REQUIRE(std::holds_alternative<bh61::core::JoinResponse>(parsed3));
  BH61_REQUIRE(std::get<bh61::core::JoinResponse>(parsed3) == response);

  response.format = 4;
  response.remote_public_hash = {0xa0, 0xa1, 0xa2, 0xa3};
  const auto format4 = bh61::core::encode_join_response(response);
  BH61_REQUIRE(format4.size() == 73U);
  const auto parsed4 = bh61::core::parse_join_response(format4);
  BH61_REQUIRE(std::holds_alternative<bh61::core::JoinResponse>(parsed4));
  BH61_REQUIRE(std::get<bh61::core::JoinResponse>(parsed4) == response);

  response.format = 5;
  const auto format5 = bh61::core::encode_join_response(response);
  BH61_REQUIRE(format5.size() == 75U);
  BH61_REQUIRE(format5[69] == 0U && format5[70] == 4U);
  const auto parsed5 = bh61::core::parse_join_response(format5);
  BH61_REQUIRE(std::holds_alternative<bh61::core::JoinResponse>(parsed5));
  BH61_REQUIRE(std::get<bh61::core::JoinResponse>(parsed5) == response);
}

BH61_TEST("join response rejects truncation and unknown formats") {
  bh61::core::JoinResponse response{};
  for (const auto format : {3U, 4U, 5U}) {
    response.format = static_cast<std::uint8_t>(format);
    const auto encoded = bh61::core::encode_join_response(response);
    for (std::size_t size = 0; size < encoded.size(); ++size) {
      BH61_REQUIRE(is_parse_error(bh61::core::parse_join_response(
          std::span<const std::uint8_t>(encoded).first(size))));
    }
  }
  std::array<std::uint8_t, 85> unknown{};
  unknown[0] = 0xff;
  BH61_REQUIRE(is_parse_error(bh61::core::parse_join_response(unknown)));
}

BH61_TEST("join signature is length delimited and exact") {
  const std::vector<std::uint8_t> bytes{0x00, 0x03, 0xaa, 0xbb, 0xcc};
  const auto parsed = bh61::core::parse_join_signature(bytes);
  BH61_REQUIRE(std::holds_alternative<bh61::core::JoinSignature>(parsed));
  BH61_REQUIRE(std::get<bh61::core::JoinSignature>(parsed).signature ==
               (std::vector<std::uint8_t>{0xaa, 0xbb, 0xcc}));
  BH61_REQUIRE(bh61::core::encode_join_signature(
                   std::get<bh61::core::JoinSignature>(parsed)) == bytes);
  auto trailing = bytes;
  trailing.push_back(0xdd);
  BH61_REQUIRE(is_parse_error(bh61::core::parse_join_signature(trailing)));
}

BH61_TEST("event and acknowledgement codecs preserve recovered fields") {
  const std::vector<std::uint8_t> event_bytes{
      0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04,
      0x00, 0x00, 0x00, 0x01, 0x7f, 0x02, 0xaa, 0xbb};
  const auto event_result = bh61::core::parse_event_request(event_bytes);
  BH61_REQUIRE(std::holds_alternative<bh61::core::EventRequest>(event_result));
  const auto& event = std::get<bh61::core::EventRequest>(event_result);
  BH61_REQUIRE(event.type == 6U && event.active && event.age_ms == 0U);
  BH61_REQUIRE(event.tlvs.size() == 2U);
  BH61_REQUIRE(bh61::core::encode_event_request(event) == event_bytes);

  const std::array<std::uint8_t, 4> ack_bytes{0x12, 0x34, 0x56, 0x78};
  const auto ack_result = bh61::core::parse_sense_ack_request(ack_bytes);
  BH61_REQUIRE(std::holds_alternative<bh61::core::SenseAckRequest>(ack_result));
  const auto& ack = std::get<bh61::core::SenseAckRequest>(ack_result);
  BH61_REQUIRE(ack.event_id == 0x12345678U);
  BH61_REQUIRE(bh61::core::encode_sense_ack_request(ack) ==
               std::vector<std::uint8_t>(ack_bytes.begin(), ack_bytes.end()));
}

BH61_TEST("acknowledgement response preserves status and message") {
  const std::array<std::uint8_t, 7> bytes{
      0x00, 0x00, 0x00, 0x13, 'b', 'a', 'd'};
  const auto parsed = bh61::core::parse_ack_response(bytes);
  BH61_REQUIRE(std::holds_alternative<bh61::core::AckResponse>(parsed));
  const auto& response = std::get<bh61::core::AckResponse>(parsed);
  BH61_REQUIRE(response.status == 0x13U);
  BH61_REQUIRE(response.message ==
               (std::vector<std::uint8_t>{'b', 'a', 'd'}));
  BH61_REQUIRE(bh61::core::encode_ack_response(response) ==
               (std::vector<std::uint8_t>(bytes.begin(), bytes.end())));
}

BH61_TEST("named sensor events expose only intended semantics") {
  struct Expected {
    std::string_view name;
    std::uint8_t type;
    bool active;
  };
  constexpr std::array expected{
      Expected{"tamper", 1U, true},
      Expected{"water-dry", 2U, false},
      Expected{"water-wet", 2U, true},
      Expected{"motion", 3U, true},
      Expected{"contact-closed", 4U, false},
      Expected{"contact-opened", 4U, true},
      Expected{"glass-break", 5U, true},
      Expected{"heartbeat", 6U, true},
      Expected{"supervision", 6U, true},
      Expected{"panic-pressed", 7U, true},
      Expected{"heartbeat-secondary", 8U, true},
      Expected{"relay-input-closed", 9U, false},
      Expected{"relay-input-opened", 9U, true},
      Expected{"reverse-contact-opened", 10U, false},
      Expected{"reverse-contact-closed", 10U, true},
  };
  for (const auto& value : expected) {
    const auto semantic = bh61::core::event_semantic(value.name);
    BH61_REQUIRE(semantic.has_value());
    BH61_REQUIRE(semantic->type == value.type);
    BH61_REQUIRE(semantic->active == value.active);
  }
  BH61_REQUIRE(!bh61::core::event_semantic("unknown").has_value());
  BH61_REQUIRE(!bh61::core::event_semantic("reset").has_value());
  BH61_REQUIRE(!bh61::core::event_semantic("dfu").has_value());
}

BH61_TEST("event codecs reject invalid active values and TLV boundaries") {
  const std::vector<std::uint8_t> event_bytes{
      0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04,
      0x00, 0x00, 0x00, 0x01};
  auto bad_active = event_bytes;
  bad_active[1] = 2;
  BH61_REQUIRE(is_parse_error(bh61::core::parse_event_request(bad_active)));
  auto bad_length = event_bytes;
  bad_length[7] = 5;
  BH61_REQUIRE(is_parse_error(bh61::core::parse_event_request(bad_length)));
  BH61_REQUIRE(is_parse_error(bh61::core::parse_sense_ack_request(
      std::array<std::uint8_t, 3>{0, 0, 1})));
}
