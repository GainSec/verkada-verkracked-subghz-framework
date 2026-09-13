#include "bh61/evidence/lab_event.hpp"
#include "test_harness.hpp"

#include <string>

BH61_TEST("lab event round trip retains schema fields and extensions") {
  const std::string encoded =
      R"({"schema":"bh61.lab.event/v1","event_id":"evt-1","utc":"2026-09-06T15:04:05.123456Z","monotonic_ns":42,"source":"sdr-framework","device_id":"hub-1","sensor_id":"zone-7","correlation_id":"capture-42","type":"rf.tx","direction":"outbound","evidence_refs":["capture-42.tx.json"],"payload":{"sample_count":13400},"future_field":{"x":7}})";
  const auto event = bh61::evidence::parse_lab_event(encoded);
  BH61_REQUIRE(event.event_id == "evt-1");
  BH61_REQUIRE(event.monotonic_ns == 42U);
  BH61_REQUIRE(event.extensions.at("future_field") == R"({"x":7})");
  const auto round_trip = bh61::evidence::serialize_lab_event(event);
  BH61_REQUIRE(round_trip.find(R"("schema":"bh61.lab.event/v1")") !=
               std::string::npos);
  BH61_REQUIRE(round_trip.find(R"("future_field":{"x":7})") !=
               std::string::npos);
  BH61_REQUIRE(bh61::evidence::parse_lab_event(round_trip).event_id ==
               "evt-1");
}

BH61_TEST("lab event parser rejects an invalid contract") {
  bool rejected = false;
  try {
    (void)bh61::evidence::parse_lab_event(
        R"({"schema":"wrong","event_id":"evt-1","utc":"2026-09-06T15:04:05Z","source":"test","type":"rf.rx","direction":"inbound","payload":{}})");
  } catch (...) {
    rejected = true;
  }
  BH61_REQUIRE(rejected);
}
