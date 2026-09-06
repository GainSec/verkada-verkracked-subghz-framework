import hashlib
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "fixtures" / "manifest.json"


class FixtureProvenanceTests(unittest.TestCase):
    def test_every_declared_fixture_exists_and_matches_its_digest(self):
        document = json.loads(MANIFEST.read_text())
        self.assertEqual("bh-series-public-fixtures/v1", document["schema"])
        for fixture in document["fixtures"]:
            path = ROOT / fixture["path"]
            self.assertTrue(path.is_file(), fixture["path"])
            self.assertEqual(
                fixture["sha256"], hashlib.sha256(path.read_bytes()).hexdigest()
            )

    def test_every_fixture_has_public_provenance_and_claim_boundaries(self):
        fixtures = json.loads(MANIFEST.read_text())["fixtures"]
        allowed = {"synthetic", "independently-reconstructed"}
        for fixture in fixtures:
            self.assertIn(fixture["classification"], allowed)
            self.assertTrue(fixture["description"])
            self.assertTrue(fixture["identifiers_sanitized"])
            self.assertFalse(fixture["exact_observed"])
            self.assertFalse(fixture["physically_validated"])
            self.assertTrue(fixture["analytical_only"])


if __name__ == "__main__":
    unittest.main()
