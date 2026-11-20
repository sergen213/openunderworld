#!/usr/bin/env python3
"""Parsers of tools/progress/progress.py against the real repository documents.

    python3 tools/progress/test_progress.py
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import progress as p  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
TREE = p.WorkTree(ROOT)


class RealDocuments(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d = p.collect(TREE, strict=True)

    def test_brief_summary(self):
        b = self.d["brief"]
        self.assertGreaterEqual(len(b["sections"]), 10)
        t = b["total"]
        self.assertGreater(t["met"], 0)
        self.assertEqual(sum(s["met"] for s in b["sections"]), t["met"])
        self.assertTrue(0 < b["met_pct"] <= 100)

    def test_bugs_index(self):
        bugs = self.d["bugs"]
        self.assertGreater(bugs["total"], 50)
        self.assertEqual(sum(bugs["counts"].values()), bugs["total"])
        self.assertEqual(sum(sum(c.values()) for c in bugs["by_prefix"].values()), bugs["total"])
        self.assertIn("OU-PLY", bugs["by_prefix"])
        self.assertGreater(bugs["counts"]["fixed"], 0)
        self.assertGreater(bugs["counts"]["open"], 0)

    def test_comparison(self):
        c = self.d["comparison"]
        self.assertGreaterEqual(len(c["sections"]), 5)
        self.assertGreater(c["total"], 100)
        self.assertEqual(sum(c["counts"].values()), c["total"])
        self.assertGreater(c["matching"], 0)
        self.assertGreater(c["counts"]["unmeasured"], 0)
        self.assertLessEqual(c["matching"], c["measured"])

    def test_chapters(self):
        ch = self.d["chapters"]
        self.assertEqual([c["chapter"] for c in ch["chapters"]], list(range(1, p.CHAPTERS + 1)))
        for c in ch["chapters"]:
            self.assertGreater(c["rows"], 0, c["title"])
            self.assertIsNotNone(c["selftests"], c["title"])
            self.assertIn(c["bucket"], p.BUCKETS)
        self.assertGreater(ch["counts"]["verified"], 0)
        self.assertTrue(0 < ch["implemented_pct"] <= 100)
        self.assertLessEqual(ch["blocked"], ch["count"])

    def test_reverse_engineering(self):
        r, v = self.d["re"], self.d["verification"]
        self.assertGreater(r["total"], 10000)
        self.assertEqual(sum(r["counts"].values()), r["total"])
        self.assertGreater(r["counts"]["Reimplemented-cited"], 0)
        self.assertGreater(v["total"], 0)
        self.assertGreater(v["counts"]["match"], 0)

    def test_tests(self):
        t = self.d["tests"]
        self.assertGreater(t["gtest_macros"], 100)
        self.assertGreater(t["selftest_scenarios"], 10)

    def test_render(self):
        with tempfile.TemporaryDirectory() as out:
            p.render(self.d, {"sha": "0" * 40, "date": "2026-01-01"}, Path(out))
            names = {f.name for f in Path(out).iterdir()}
            for n in ("index.html", "progress.json", "progress.svg", *p.badges(self.d)):
                self.assertIn(n, names)


class FailsLoudly(unittest.TestCase):
    def test_changed_brief_header(self):
        text = TREE.read(p.BRIEF).replace("| Section | Met | Partly |", "| Section | Done | Partly |")
        with self.assertRaises(p.ParseError):
            p.parse_brief(text)

    def test_unknown_bug_status(self):
        text = TREE.read(p.BUGS).replace("| Open | pre-existing |", "| Parked | pre-existing |", 1)
        with self.assertRaises(p.ParseError):
            p.parse_bugs(text)

    def test_unknown_comparison_status(self):
        text = TREE.read(p.COMPARISON).replace("| matches |", "| equal |", 1)
        with self.assertRaises(p.ParseError):
            p.parse_comparison(text)

    def test_unknown_requirement_status(self):
        text = TREE.read(f"{p.MATRIX_DIR}/01_prologue.md").replace("| VERIFIED AGAINST ORIGINAL |", "| DONE |", 1)
        with self.assertRaises(p.ParseError):
            p.parse_matrix(text)

    def test_missing_column(self):
        with self.assertRaises(p.ParseError):
            p.parse_functions("entry\tsize\n00401000\t31\n")


if __name__ == "__main__":
    unittest.main()
