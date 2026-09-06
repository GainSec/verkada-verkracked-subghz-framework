#include "bh61/app/arguments.hpp"
#include "bh61/radio/device_factory.hpp"

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  bh61::radio::NativeDeviceFactory factory;
  return bh61::app::run_cli(
      arguments, std::cout, std::cerr,
      bh61::app::CliEnvironment{nullptr, &factory});
}
