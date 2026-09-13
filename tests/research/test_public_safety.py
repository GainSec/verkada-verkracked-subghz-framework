import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class PublicSafetyTests(unittest.TestCase):
    def test_public_cli_excludes_adversarial_commands(self):
        cli = (ROOT / "src" / "apps" / "arguments.cpp").read_text()
        for command in (
            'name == "mutate"',
            'name == "mutate-transmit"',
            'name == "replay"',
            'name == "sensor-counter-resync-test"',
        ):
            self.assertNotIn(command, cli)

    def test_public_tree_excludes_fuzzing_mutation_and_private_evidence(self):
        forbidden = (
            "include/bh61/fuzz",
            "src/fuzz",
            "include/bh61/core/mutation.hpp",
            "src/core/mutation.cpp",
            "research/efr32/mutate.py",
            "research/efr32/corpus/mutations",
            "evidence",
        )
        for relative in forbidden:
            self.assertFalse((ROOT / relative).exists(), relative)

    def test_public_cli_contains_safe_stateful_sensor_commands(self):
        cli = (ROOT / "src" / "apps" / "arguments.cpp").read_text()
        for command in (
            "sensor-create",
            "sensor-status",
            "sensor-join",
            "sensor-event",
            "sensor-rpc-read",
            "sensor-retire",
        ):
            self.assertIn(f'name == "{command}"', cli)

    def test_public_cli_has_only_normal_mac_ack_behavior(self):
        cli = (ROOT / "src" / "apps" / "arguments.cpp").read_text()
        self.assertNotIn("--downlink-mac-ack-mode", cli)
        for mode in ("wrong-sequence", "duplicate", "late", "suppress"):
            self.assertNotIn(f'== "{mode}"', cli)

    def test_release_notes_state_public_v2_exclusions(self):
        notes = (ROOT / "PUBLIC_RELEASE_NOTES.md").read_text().lower()
        for phrase in ("fuzzing", "shell execution", "private research"):
            self.assertIn(phrase, notes)

    def test_preflight_is_parameterized_and_contains_no_private_lab_values(self):
        preflight = (ROOT / "tools" / "bh61_isolation_preflight.sh").read_text()
        for required in (
            "--hub-ip",
            "--hub-mac",
            "--dns-ip",
            "--domains-file",
            "--emulator-health-url",
            "--b210-serial",
            "--fpga-sha256",
            "--uart-oracle",
        ):
            self.assertIn(required, preflight)
        for private_value in ("internal.example", "192" + ".168."):
            self.assertNotIn(private_value, preflight)

    def test_stateful_guide_and_fixture_match_the_public_boundary(self):
        guide = (ROOT / "docs" / "STATEFUL-SENSORS.md").read_text().lower()
        for phrase in (
            "sensor-private.key",
            "sensor-create",
            "sensor-join",
            "sensor-event",
            "sensor-rpc-read",
            "sensor-retire",
        ):
            self.assertIn(phrase, guide)
        self.assertTrue(
            (ROOT / "fixtures" / "session" / "synthetic-join-public.json").is_file()
        )
        self.assertFalse((ROOT / "fixtures" / "protocol" / "reset-req.hex").exists())

    def test_native_transmit_is_opt_in_at_build_and_runtime(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        options = (ROOT / "cmake" / "Bh61Options.cmake").read_text()
        cli = (ROOT / "src" / "apps" / "arguments.cpp").read_text()
        self.assertIn("option(BH61_ENABLE_TX", options)
        self.assertIn("BH61_ENABLE_TX", cmake)
        self.assertIn("--enable-tx", cli)
        self.assertIn("transmit requires --enable-tx", cli)

    def test_advertised_sdr_backends_have_implementations(self):
        options = (ROOT / "cmake" / "Bh61Options.cmake").read_text()
        for option, implementation in (
            ("BH61_ENABLE_UHD", "src/radio/libuhd_transport.cpp"),
            ("BH61_ENABLE_RTLSDR", "src/radio/librtlsdr_transport.cpp"),
            ("BH61_ENABLE_HACKRF", "src/radio/libhackrf_transport.cpp"),
        ):
            self.assertIn(option, options)
            self.assertTrue((ROOT / implementation).is_file())


if __name__ == "__main__":
    unittest.main()
