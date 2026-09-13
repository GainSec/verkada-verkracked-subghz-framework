#include "bh61/core/sensor_model.hpp"
#include "test_harness.hpp"

#include <sstream>

BH61_TEST("synthetic sensor tracks enrollment counter and alarm state") {
  bh61::core::SensorModel sensor("zone-7");
  const auto enrolled = sensor.apply({bh61::core::SensorActionType::Enroll, 0});
  BH61_REQUIRE(enrolled.type == "sensor.enrolled");
  const auto opened = sensor.apply({bh61::core::SensorActionType::Contact, 1});
  BH61_REQUIRE(opened.counter == 1U);
  BH61_REQUIRE(sensor.state().contact_open);
  const auto tamper = sensor.apply({bh61::core::SensorActionType::Tamper, 1});
  BH61_REQUIRE(tamper.counter == 2U);
  BH61_REQUIRE(sensor.state().tamper_open);
  const auto retransmit = sensor.apply({bh61::core::SensorActionType::Retransmit, 0});
  BH61_REQUIRE(retransmit.counter == 2U);
  BH61_REQUIRE(retransmit.retransmission);
}

BH61_TEST("sensor scenario parser preserves ordered explicit actions") {
  std::istringstream input("sensor zone-7\nenroll\ncontact open\nbattery 42\nsupervision\n");
  const auto scenario = bh61::core::parse_sensor_scenario(input);
  BH61_REQUIRE(scenario.sensor_id == "zone-7");
  BH61_REQUIRE(scenario.actions.size() == 4U);
  BH61_REQUIRE(scenario.actions[2].type == bh61::core::SensorActionType::Battery);
  BH61_REQUIRE(scenario.actions[2].value == 42);
}
