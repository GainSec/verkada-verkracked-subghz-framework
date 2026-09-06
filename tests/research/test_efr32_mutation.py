import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from research.efr32.model import build_plaintext_frame, build_v4_join_body


REPO = Path(__file__).resolve().parents[2]
MUTATOR = REPO / "research" / "efr32" / "mutate.py"
CORPUS_GENERATOR = REPO / "research" / "efr32" / "generate_corpus.py"


class Efr32MutationTests(unittest.TestCase):
    def run_mutator(self, directory: Path) -> dict:
        seed = directory / "seed.bin"
        seed.write_bytes(build_plaintext_frame(
            7, build_v4_join_body(b"seed"), bytes.fromhex("0102030405060708")
        ))
        output = directory / "out"
        subprocess.run(
            ["python3", str(MUTATOR), "--seed", str(seed), "--output", str(output),
             "--limit", "96", "--random-seed", "0x0b61"],
            check=True,
        )
        manifest = json.loads((output / "manifest.json").read_text())
        for case in manifest["cases"]:
            content = (output / case["path"]).read_bytes()
            self.assertEqual(case["sha256"], hashlib.sha256(content).hexdigest())
            self.assertLessEqual(len(content), manifest["limits"]["max_input_bytes"])
        return manifest

    def test_mutations_are_bounded_unique_and_cover_declared_dimensions(self):
        with tempfile.TemporaryDirectory() as name:
            manifest = self.run_mutator(Path(name))
        self.assertLessEqual(len(manifest["cases"]), 96)
        hashes = [case["sha256"] for case in manifest["cases"]]
        self.assertEqual(len(hashes), len(set(hashes)))
        dimensions = {case["dimension"] for case in manifest["cases"]}
        self.assertTrue({"length", "type", "counter", "id", "truncate",
                         "extend", "repeat"}.issubset(dimensions))
        self.assertEqual("offline-only", manifest["scope"])

    def test_manifest_and_cases_are_deterministic(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            first = root / "first"
            second = root / "second"
            first.mkdir()
            second.mkdir()
            a = self.run_mutator(first)
            b = self.run_mutator(second)
        self.assertEqual(a, b)

    def test_committed_corpus_is_reproducible(self):
        with tempfile.TemporaryDirectory() as name:
            output = Path(name) / "corpus"
            subprocess.run(
                ["python3", str(CORPUS_GENERATOR), "--output", str(output)],
                check=True,
            )
            committed = REPO / "research" / "efr32" / "corpus"
            expected = json.loads((committed / "manifest.json").read_text())
            actual = json.loads((output / "manifest.json").read_text())
            self.assertEqual(expected, actual)
            for record in actual["artifacts"]:
                self.assertEqual("synthetic", record["classification"])
                self.assertTrue(record["identifiers_sanitized"])
                self.assertFalse(record["exact_observed"])
                self.assertFalse(record["physically_validated"])
                self.assertTrue(record["analytical_only"])
                self.assertTrue(record["description"])
                self.assertEqual(
                    (committed / record["path"]).read_bytes(),
                    (output / record["path"]).read_bytes(),
                )


if __name__ == "__main__":
    unittest.main()
