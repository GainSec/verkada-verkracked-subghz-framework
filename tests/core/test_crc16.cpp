#include "bh61/core/bytes.hpp"
#include "bh61/core/crc16.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>

BH61_TEST("VCMP CRC matches the fresh scan-info vector") {
  constexpr std::array<std::uint8_t, 9> input{
      0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
  BH61_REQUIRE(bh61::core::vcmp_crc16(input) == 0x338b);
}

BH61_TEST("VCMP CRC matches the independently recovered ResetReq vector") {
  constexpr std::array<std::uint8_t, 10> input{
      0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00};
  BH61_REQUIRE(bh61::core::vcmp_crc16(input) == 0x5530);
}

BH61_TEST("radio FCS uses the on-air reflected remainder byte order") {
  constexpr std::array<std::uint8_t, 24> psdu{
      0x41, 0xc8, 0x00, 0xff, 0x01, 0x01, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b,
      0x00, 0x33, 0x8b, 0x00, 0x00, 0x01, 0x00, 0x00};
  BH61_REQUIRE(bh61::core::radio_fcs16(psdu) == 0x705c);
}

BH61_TEST("radio FCS matches the live BH61 scan-info frame") {
  constexpr std::array<std::uint8_t, 24> psdu{
      0x41, 0xc8, 0xe4, 0xff, 0x01, 0x02, 0x00, 0x6a,
      0xce, 0x40, 0xfe, 0xff, 0x9c, 0xc5, 0x70, 0x0b,
      0x00, 0x92, 0xc1, 0x00, 0x00, 0x01, 0x00, 0x0a};
  BH61_REQUIRE(bh61::core::radio_fcs16(psdu) == 0x030e);
}

BH61_TEST("16-bit byte loads and stores have explicit endianness") {
  constexpr std::array<std::uint8_t, 2> bytes{0x12, 0x34};
  BH61_REQUIRE(bh61::core::load_be16(bytes) == 0x1234);
  BH61_REQUIRE(bh61::core::load_le16(bytes) == 0x3412);
  BH61_REQUIRE(bh61::core::store_be16(0x1234) == bytes);
  constexpr std::array<std::uint8_t, 2> reversed{0x34, 0x12};
  BH61_REQUIRE(bh61::core::store_le16(0x1234) == reversed);
}
