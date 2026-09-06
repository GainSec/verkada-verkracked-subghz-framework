option(BH61_ENABLE_HARDWARE "Enable native SDR hardware backends" ON)
option(BH61_ENABLE_HACKRF "Enable the HackRF backend when available" ${BH61_ENABLE_HARDWARE})
option(BH61_REQUIRE_HACKRF "Fail configuration unless native HackRF development files are available" OFF)
