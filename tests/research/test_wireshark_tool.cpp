#include "test_harness.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

BH61_TEST("Wireshark dissector registers DLT_USER0 and recovered fields") {
  const auto path = std::filesystem::path(BH61_SOURCE_DIR) /
                    "tools/wireshark/bh61.lua";
  std::ifstream stream(path);
  BH61_REQUIRE(stream.good());
  const std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
  BH61_REQUIRE(text.find("wtap_encap.USER0") != std::string::npos);
  BH61_REQUIRE(text.find("bh61.phr.length") != std::string::npos);
  BH61_REQUIRE(text.find("bh61.radio_fcs") != std::string::npos);
  BH61_REQUIRE(text.find("bh61.vmac.sequence") != std::string::npos);
  BH61_REQUIRE(text.find("bh61.vcmp.type") != std::string::npos);
}
