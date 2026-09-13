#pragma once

#include "bh61/radio/device.hpp"

#include <complex>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bh61::radio {

struct UhdIdentity {
  std::string serial;
  std::string product;
  std::string name;
};

struct UhdConfiguration {
  std::string serial;
  std::string fpga_path;
  double sample_rate{4'000'000.0};
  double center_frequency_hz{915'350'000.0};
  double rx_gain_db{20.0};
  double tx_gain_db{0.0};
  std::string rx_antenna;
  std::string tx_antenna;
  std::size_t receive_frame_count{1'024U};
  std::size_t rx_channel{};
  std::size_t tx_channel{};
  bool enable_receive{true};
  bool enable_coherent_receive{true};
  bool enable_transmit{true};
};

class UhdTransport {
 public:
  virtual ~UhdTransport() = default;
  virtual auto enumerate() -> std::vector<UhdIdentity> = 0;
  virtual void open(const UhdConfiguration& configuration) = 0;
  virtual void start_receive_stream() = 0;
  virtual void stop_receive_stream() = 0;
  virtual auto receive(std::size_t maximum_samples) -> SampleBlock = 0;
  virtual auto receive_pair(std::size_t maximum_samples)
      -> CoherentSamplePair = 0;
  virtual auto current_time_ns() const -> std::uint64_t {
    throw std::logic_error("UHD transport does not expose its hardware clock");
  }
  virtual void transmit(std::span<const std::complex<float>> samples,
                        std::uint64_t requested_time_ns) = 0;
  virtual void close() = 0;
};

}  // namespace bh61::radio
