#pragma once

#include "bh61/radio/uhd_transport.hpp"

#include <memory>

namespace bh61::radio {

class LibuhdTransport final : public UhdTransport {
 public:
  LibuhdTransport();
  ~LibuhdTransport() override;
  auto enumerate() -> std::vector<UhdIdentity> override;
  void open(const UhdConfiguration& configuration) override;
  auto receive(std::size_t maximum_samples) -> SampleBlock override;
  auto receive_pair(std::size_t maximum_samples)
      -> CoherentSamplePair override;
  void transmit(std::span<const std::complex<float>> samples,
                std::uint64_t requested_time_ns) override;
  void close() override;

 private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace bh61::radio
