#include "bh61/version.hpp"

#include <array>

namespace bh61 {

auto version() noexcept -> std::string_view { return "0.1.0"; }

auto build_capabilities() noexcept -> std::span<const BuildCapability> {
  static constexpr std::array capabilities{
      BuildCapability{"file", true, true},
#if defined(BH61_HAVE_HACKRF)
      BuildCapability{"hackrf-rx", true, false},
#if defined(BH61_ENABLE_TX)
      BuildCapability{"hackrf-tx", false, true},
#endif
#endif
#if defined(BH61_HAVE_UHD)
      BuildCapability{"uhd-rx", true, false},
#if defined(BH61_ENABLE_TX)
      BuildCapability{"uhd-tx", false, true},
#endif
#endif
#if defined(BH61_HAVE_RTLSDR)
      BuildCapability{"rtlsdr", true, false},
#endif
  };
  return capabilities;
}

}  // namespace bh61
