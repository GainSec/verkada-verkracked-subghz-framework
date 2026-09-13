#pragma once

#include "bh61/radio/device.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bh61::radio {

struct RtlSdrIdentity {
  std::string serial;
  std::string name;
  std::uint32_t index{};
};

struct RtlSdrConfiguration {
  std::string serial;
  std::uint32_t sample_rate{2'400'000U};
  std::uint32_t center_frequency_hz{915'350'000U};
  int tuner_gain_tenths_db{};
};

class RtlSdrTransport {
 public:
  virtual ~RtlSdrTransport() = default;
  virtual auto enumerate() -> std::vector<RtlSdrIdentity> = 0;
  virtual void open(const RtlSdrConfiguration& configuration) = 0;
  virtual auto receive(std::size_t maximum_samples) -> SampleBlock = 0;
  virtual void close() = 0;
};

}  // namespace bh61::radio
