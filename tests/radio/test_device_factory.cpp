#include "bh61/radio/device_factory.hpp"
#include "test_harness.hpp"

#include <vector>

namespace {

class FixedFactory final : public bh61::radio::DeviceFactory {
 public:
  std::size_t enumerate_calls{};

  auto enumerate() -> std::vector<bh61::radio::DeviceDescriptor> override {
    ++enumerate_calls;
    return {{"hackrf",
             "00000000000000000000000000000001",
             "HackRF Pro",
             "git-9039eb06 (API:1.09)",
             "r1.2",
             {true, false, false, false, false,
              bh61::radio::SampleFormat::SignedInt8, 2'000'000U,
              20'000'000U}}};
  }
};

}  // namespace

BH61_TEST("device factory descriptors preserve attached hardware identity") {
  FixedFactory factory;
  const auto devices = factory.enumerate();
  BH61_REQUIRE(factory.enumerate_calls == 1U);
  BH61_REQUIRE(devices.size() == 1U);
  BH61_REQUIRE(devices[0].backend == "hackrf");
  BH61_REQUIRE(devices[0].serial ==
               "00000000000000000000000000000001");
  BH61_REQUIRE(devices[0].board_name == "HackRF Pro");
  BH61_REQUIRE(devices[0].firmware_version ==
               "git-9039eb06 (API:1.09)");
  BH61_REQUIRE(devices[0].hardware_revision == "r1.2");
  BH61_REQUIRE(devices[0].capabilities.rx);
  BH61_REQUIRE(!devices[0].capabilities.tx);
}
