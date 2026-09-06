#pragma once

#include <span>
#include <string_view>

namespace bh61 {

struct BuildCapability {
  std::string_view name;
  bool rx{};
  bool tx{};
};

auto version() noexcept -> std::string_view;
auto build_capabilities() noexcept -> std::span<const BuildCapability>;

}  // namespace bh61
