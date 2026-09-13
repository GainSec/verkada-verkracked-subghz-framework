#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace bh61::core {

enum class SensorActionType { Enroll, Contact, Motion, Tamper, Battery, Supervision, Retransmit };
struct SensorAction { SensorActionType type; int value{}; };
struct SensorState {
  std::string sensor_id;
  bool enrolled{};
  std::uint16_t counter{};
  bool contact_open{};
  bool motion_active{};
  bool tamper_open{};
  std::uint8_t battery_percent{100U};
  std::uint32_t supervision_sequence{};
};
struct SensorObservation {
  std::string type;
  std::uint16_t counter{};
  bool retransmission{};
  std::string payload_json;
};
struct SensorScenario { std::string sensor_id; std::vector<SensorAction> actions; };

class SensorModel {
 public:
  explicit SensorModel(std::string sensor_id);
  auto apply(SensorAction action) -> SensorObservation;
  auto state() const noexcept -> const SensorState&;
 private:
  SensorState state_;
  SensorObservation last_;
};

auto parse_sensor_scenario(std::istream& input) -> SensorScenario;

}  // namespace bh61::core
