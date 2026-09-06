#include "bh61/core/frame.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <variant>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 27> exact_type11_frame{
    0x1a, 0x41, 0xc8, 0x00, 0xff, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00,
    0x33, 0x8b, 0x00, 0x00, 0x01, 0x00, 0x00, 0x0e, 0x3a};

}  // namespace

BH61_TEST("radio frame parser validates PHR and radio FCS") {
  const auto result = bh61::core::parse_radio_frame(exact_type11_frame);
  BH61_REQUIRE(std::holds_alternative<bh61::core::RadioFrame>(result));
  const auto& frame = std::get<bh61::core::RadioFrame>(result);
  BH61_REQUIRE(frame.phr == 0x1a);
  BH61_REQUIRE(frame.psdu.size() == 24);
  BH61_REQUIRE(frame.received_fcs == 0x0e3a);
  BH61_REQUIRE(frame.computed_fcs == 0x0e3a);
}

BH61_TEST("radio frame encodes byte-for-byte") {
  const auto parsed = bh61::core::parse_radio_frame(exact_type11_frame);
  BH61_REQUIRE(std::holds_alternative<bh61::core::RadioFrame>(parsed));
  const auto encoded =
      bh61::core::encode_radio_frame(std::get<bh61::core::RadioFrame>(parsed));
  BH61_REQUIRE(encoded == std::vector<std::uint8_t>(exact_type11_frame.begin(),
                                                    exact_type11_frame.end()));
}

BH61_TEST("radio frame rejects truncation and excess bytes") {
  std::vector<std::uint8_t> truncated(exact_type11_frame.begin(),
                                      exact_type11_frame.end() - 1);
  const auto short_result = bh61::core::parse_radio_frame(truncated);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(short_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(short_result).code ==
               bh61::core::ParseErrorCode::LengthMismatch);

  std::vector<std::uint8_t> excess(exact_type11_frame.begin(),
                                   exact_type11_frame.end());
  excess.push_back(0x00);
  const auto excess_result = bh61::core::parse_radio_frame(excess);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(excess_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(excess_result).code ==
               bh61::core::ParseErrorCode::LengthMismatch);
}

BH61_TEST("radio frame rejects impossible PHR and bad FCS") {
  auto invalid_length = exact_type11_frame;
  invalid_length[0] = 0x01;
  const auto length_result = bh61::core::parse_radio_frame(invalid_length);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(length_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(length_result).code ==
               bh61::core::ParseErrorCode::InvalidLength);

  auto corrupt = exact_type11_frame;
  corrupt[20] ^= 0x01;
  const auto crc_result = bh61::core::parse_radio_frame(corrupt);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(crc_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(crc_result).code ==
               bh61::core::ParseErrorCode::BadFcs);
}

BH61_TEST("radio frame enforces the recovered PHR range 5 through 127") {
  const std::vector<std::uint8_t> below_minimum{0x04, 0x00, 0x00, 0x00,
                                                0x00};
  const auto low_result = bh61::core::parse_radio_frame(below_minimum);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(low_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(low_result).code ==
               bh61::core::ParseErrorCode::InvalidLength);

  std::vector<std::uint8_t> above_maximum(129, 0x00);
  above_maximum[0] = 0x80;
  const auto high_result = bh61::core::parse_radio_frame(above_maximum);
  BH61_REQUIRE(std::holds_alternative<bh61::core::ParseError>(high_result));
  BH61_REQUIRE(std::get<bh61::core::ParseError>(high_result).code ==
               bh61::core::ParseErrorCode::InvalidLength);

  bool rejected_short_encode = false;
  try {
    static_cast<void>(bh61::core::encode_radio_frame(
        bh61::core::RadioFrame{0, {0x00, 0x00}, 0, 0}));
  } catch (const std::invalid_argument&) {
    rejected_short_encode = true;
  }
  BH61_REQUIRE(rejected_short_encode);

  bool rejected_large_encode = false;
  try {
    static_cast<void>(bh61::core::encode_radio_frame(
        bh61::core::RadioFrame{0, std::vector<std::uint8_t>(126, 0), 0, 0}));
  } catch (const std::invalid_argument&) {
    rejected_large_encode = true;
  }
  BH61_REQUIRE(rejected_large_encode);
}
