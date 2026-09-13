#include "bh61/core/sensor_model.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace bh61::core {

SensorModel::SensorModel(std::string sensor_id) {
  if (sensor_id.empty()) throw std::invalid_argument("sensor ID is empty");
  state_.sensor_id = std::move(sensor_id);
}
auto SensorModel::state() const noexcept -> const SensorState& { return state_; }
auto SensorModel::apply(SensorAction action) -> SensorObservation {
  if (action.type != SensorActionType::Enroll && !state_.enrolled) {
    throw std::logic_error("sensor must enroll before state changes");
  }
  SensorObservation observation;
  observation.counter = state_.counter;
  switch (action.type) {
    case SensorActionType::Enroll:
      state_.enrolled = true;
      observation.type = "sensor.enrolled";
      observation.payload_json = "{\"enrolled\":true}";
      break;
    case SensorActionType::Contact:
      state_.contact_open = action.value != 0; ++state_.counter;
      observation.type = state_.contact_open ? "sensor.contact.open" : "sensor.contact.closed";
      observation.payload_json = std::string("{\"open\":") + (state_.contact_open ? "true}" : "false}");
      break;
    case SensorActionType::Motion:
      state_.motion_active = action.value != 0; ++state_.counter;
      observation.type = state_.motion_active ? "sensor.motion.active" : "sensor.motion.clear";
      observation.payload_json = std::string("{\"active\":") + (state_.motion_active ? "true}" : "false}");
      break;
    case SensorActionType::Tamper:
      state_.tamper_open = action.value != 0; ++state_.counter;
      observation.type = state_.tamper_open ? "sensor.tamper.open" : "sensor.tamper.closed";
      observation.payload_json = std::string("{\"open\":") + (state_.tamper_open ? "true}" : "false}");
      break;
    case SensorActionType::Battery:
      if (action.value < 0 || action.value > 100) throw std::invalid_argument("battery must be 0-100");
      state_.battery_percent = static_cast<std::uint8_t>(action.value); ++state_.counter;
      observation.type = "sensor.battery";
      observation.payload_json = "{\"percent\":" + std::to_string(action.value) + "}";
      break;
    case SensorActionType::Supervision:
      ++state_.supervision_sequence; ++state_.counter;
      observation.type = "sensor.supervision";
      observation.payload_json = "{\"sequence\":" + std::to_string(state_.supervision_sequence) + "}";
      break;
    case SensorActionType::Retransmit:
      if (last_.type.empty()) throw std::logic_error("nothing to retransmit");
      observation = last_;
      observation.retransmission = true;
      return observation;
  }
  observation.counter = state_.counter;
  last_ = observation;
  return observation;
}

auto parse_sensor_scenario(std::istream& input) -> SensorScenario {
  SensorScenario scenario;
  std::string line;
  std::size_t line_number{};
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string action, value, extra;
    fields >> action >> value >> extra;
    if (!extra.empty()) throw std::invalid_argument("too many scenario fields at line " + std::to_string(line_number));
    if (action == "sensor" && scenario.sensor_id.empty() && !value.empty()) { scenario.sensor_id = value; continue; }
    if (scenario.sensor_id.empty()) throw std::invalid_argument("scenario must start with sensor ID");
    if (action == "enroll" && value.empty()) scenario.actions.push_back({SensorActionType::Enroll, 0});
    else if (action == "contact" && (value == "open" || value == "closed")) scenario.actions.push_back({SensorActionType::Contact, value == "open"});
    else if (action == "motion" && (value == "active" || value == "clear")) scenario.actions.push_back({SensorActionType::Motion, value == "active"});
    else if (action == "tamper" && (value == "open" || value == "closed")) scenario.actions.push_back({SensorActionType::Tamper, value == "open"});
    else if (action == "battery" && !value.empty()) {
      std::size_t used{}; const auto parsed = std::stoi(value, &used); if (used != value.size()) throw std::invalid_argument("invalid battery value"); scenario.actions.push_back({SensorActionType::Battery, parsed});
    } else if (action == "supervision" && value.empty()) scenario.actions.push_back({SensorActionType::Supervision, 0});
    else if (action == "retransmit" && value.empty()) scenario.actions.push_back({SensorActionType::Retransmit, 0});
    else throw std::invalid_argument("unknown scenario action at line " + std::to_string(line_number));
  }
  if (scenario.sensor_id.empty() || scenario.actions.empty()) throw std::invalid_argument("scenario is empty");
  return scenario;
}

}  // namespace bh61::core
