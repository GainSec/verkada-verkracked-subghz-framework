import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class PublicSafetyTests(unittest.TestCase):
    def test_build_does_not_advertise_unimplemented_native_transmit(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        options = (ROOT / "cmake" / "Bh61Options.cmake").read_text()
        factory = (ROOT / "src" / "radio" / "device_factory.cpp").read_text()
        device = (ROOT / "src" / "radio" / "hackrf_device.cpp").read_text()
        for text in (cmake, options, factory, device):
            self.assertNotIn("BH61_ENABLE_TX", text)

    def test_unused_sdr_backends_are_not_presented_as_build_options(self):
        options = (ROOT / "cmake" / "Bh61Options.cmake").read_text()
        self.assertNotIn("BH61_ENABLE_UHD", options)
        self.assertNotIn("BH61_ENABLE_RTLSDR", options)


if __name__ == "__main__":
    unittest.main()
