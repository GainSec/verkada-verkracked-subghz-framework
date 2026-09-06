import struct
import unittest

from research.efr32.model import (
    Efr32Model,
    FrameRejected,
    build_plaintext_frame,
    build_v4_join_body,
)


class Efr32ModelTests(unittest.TestCase):
    def setUp(self):
        self.model = Efr32Model()
        self.source = bytes.fromhex("0200000000000001")

    def join(self, source: bytes, serial: bytes, sequence: int = 0, now_ms: int = 0):
        return self.model.process_frame(
            build_plaintext_frame(7, build_v4_join_body(serial), source, sequence),
            now_ms=now_ms,
        )

    def test_join_allocates_and_duplicate_identity_reinitializes_one_slot(self):
        first = self.join(self.source, b"BH61-EXAMPLE-0001")
        self.model.mark_joined(self.source, key=bytes(16), now_ms=1)
        second = self.join(
            self.source, b"BH61-EXAMPLE-0002", sequence=1, now_ms=2
        )
        self.assertEqual(1, len(self.model.peers))
        self.assertEqual("pending", self.model.peers[self.source].state)
        self.assertEqual(bytes(16), self.model.peers[self.source].key)
        self.assertEqual("allocated", first.outcome)
        self.assertEqual("reinitialized", second.outcome)

    def test_bounded_peer_model_rejects_when_all_slots_are_pending(self):
        for index in range(32):
            source = (index + 1).to_bytes(8, "big")
            self.model.process_join(source, b"BH61-EXAMPLE", now_ms=index)
        rejected = self.model.process_join(
            (9999).to_bytes(8, "big"), b"BH61-EXAMPLE", now_ms=100
        )
        self.assertEqual("pool-full", rejected.outcome)
        self.assertEqual(32, len(self.model.peers))

    def test_expiry_and_counter_window_are_deterministic(self):
        self.join(self.source, b"BH61-EXAMPLE-0001")
        self.model.mark_joined(
            self.source, bytes(16), now_ms=1, rx_expected=0xFFFE, tx_counter=7
        )
        self.assertTrue(self.model.accept_receive_counter(self.source, 0xFFFF))
        self.assertEqual(0, self.model.peers[self.source].rx_expected)
        self.assertTrue(self.model.accept_receive_counter(self.source, 0))

        body = b"\x01" + struct.pack(">H", 0x1234)
        result = self.model.process_frame(
            build_plaintext_frame(4, body, self.source, sequence=2), now_ms=2
        )
        self.assertEqual("counter-updated", result.outcome)
        self.assertEqual(0x1234, self.model.peers[self.source].tx_counter)

    def test_scan_info_is_parsed_as_observation_without_device_control_claim(self):
        result = self.model.process_frame(
            build_plaintext_frame(11, b"\x00\x00\x01\x00\x07", self.source)
        )
        self.assertEqual("scan-info-observed", result.outcome)
        self.assertEqual(0, self.model.live_channel)

    def test_invalid_frames_do_not_partially_mutate_state(self):
        valid = build_plaintext_frame(
            7, build_v4_join_body(b"BH61-EXAMPLE-0001"), self.source
        )
        for cut in range(len(valid)):
            model = Efr32Model()
            with self.assertRaises(FrameRejected):
                model.process_frame(valid[:cut])
            self.assertFalse(model.peers)


if __name__ == "__main__":
    unittest.main()
