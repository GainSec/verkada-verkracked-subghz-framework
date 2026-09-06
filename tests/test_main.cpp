#include "bh61/version.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <string_view>

BH61_TEST("version is a nonempty semantic version") {
  const std::string_view value = bh61::version();
  BH61_REQUIRE(!value.empty());
  BH61_REQUIRE(std::count(value.begin(), value.end(), '.') == 2);
}

BH61_TEST("file backend is always compiled") {
  const auto capabilities = bh61::build_capabilities();
  const auto file = std::find_if(
      capabilities.begin(), capabilities.end(), [](const auto& capability) {
        return capability.name == "file";
      });
  BH61_REQUIRE(file != capabilities.end());
  BH61_REQUIRE(file->rx);
  BH61_REQUIRE(file->tx);
}

BH61_TEST("native HackRF capability matches build configuration") {
  const auto capabilities = bh61::build_capabilities();
  const auto hackrf = std::find_if(
      capabilities.begin(), capabilities.end(), [](const auto& capability) {
        return capability.name == "hackrf-rx" ||
               capability.name == "hackrf-tx";
      });
#if defined(BH61_HAVE_HACKRF)
  BH61_REQUIRE(hackrf != capabilities.end());
#else
  BH61_REQUIRE(hackrf == capabilities.end());
#endif
}
