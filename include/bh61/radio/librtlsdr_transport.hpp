#pragma once

#include "bh61/radio/rtlsdr_transport.hpp"

#include <memory>

namespace bh61::radio {
class LibrtlSdrTransport final : public RtlSdrTransport {
 public:
  LibrtlSdrTransport();
  ~LibrtlSdrTransport() override;
  auto enumerate() -> std::vector<RtlSdrIdentity> override;
  void open(const RtlSdrConfiguration& configuration) override;
  auto receive(std::size_t maximum_samples) -> SampleBlock override;
  void close() override;
 private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};
}  // namespace bh61::radio
