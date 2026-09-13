#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bh61::evidence {

inline constexpr std::string_view lab_event_schema = "bh61.lab.event/v1";

struct LabEvent {
  std::string event_id;
  std::string utc;
  std::optional<std::uint64_t> monotonic_ns;
  std::string source;
  std::string device_id;
  std::string sensor_id;
  std::string correlation_id;
  std::string type;
  std::string direction;
  std::vector<std::string> evidence_refs;
  std::string payload_json{"{}"};
  std::map<std::string, std::string> extensions;
};

auto parse_lab_event(std::string_view json) -> LabEvent;
auto serialize_lab_event(const LabEvent& event) -> std::string;

}  // namespace bh61::evidence
