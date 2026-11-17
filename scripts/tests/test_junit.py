# Copyright 2026 The ctucanfd authors
# SPDX-License-Identifier: Apache-2.0
"""Unit tests for the parts of ctutest.py that need no target: the JUnit
writer and the test list. Run on any host:

    python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""
import os
import sys
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctutest  # noqa: E402


class JUnitTest(unittest.TestCase):
    RESULTS = [
        ("tx_lengths", "PASS", "65 lengths byte for byte"),
        ("two_nodes", "SKIP", "no --peer given"),
        ("fi_warn", "FAIL", "Fail: ewl_up 0 <expected 1> & more\nsecond line"),
        ("rtr", "PASS", ""),
    ]

    def parse(self, results):
        return ET.fromstring(ctutest.junit_xml(results).encode())

    def test_counts(self):
        suite = self.parse(self.RESULTS).find("testsuite")
        self.assertEqual(suite.get("tests"), "4")
        self.assertEqual(suite.get("failures"), "1")
        self.assertEqual(suite.get("skipped"), "1")
        self.assertEqual(suite.get("errors"), "0")
        self.assertEqual([c.get("name") for c in suite.iter("testcase")], [r[0] for r in self.RESULTS])

    def test_outcomes(self):
        cases = {c.get("name"): c for c in self.parse(self.RESULTS).iter("testcase")}
        self.assertIsNone(cases["tx_lengths"].find("failure"))
        self.assertEqual(cases["tx_lengths"].find("system-out").text, "65 lengths byte for byte")
        self.assertEqual(cases["two_nodes"].find("skipped").get("message"), "no --peer given")
        fail = cases["fi_warn"].find("failure")
        self.assertEqual(fail.get("message"), "Fail: ewl_up 0 <expected 1> & more")   # first line, escaped and back
        self.assertEqual(fail.text, self.RESULTS[2][2])
        self.assertIsNone(cases["rtr"].find("system-out"))

    def test_no_times(self):
        root = self.parse(self.RESULTS)
        for el in root.iter():
            self.assertNotIn("time", el.attrib, el.tag)

    def test_empty(self):
        suite = self.parse([]).find("testsuite")
        self.assertEqual(suite.get("tests"), "0")


class TestListTest(unittest.TestCase):
    def test_order_names_every_test_once(self):
        names = [f.__name__[2:] for f in ctutest.TESTS]
        self.assertEqual(len(ctutest.ORDER), len(set(ctutest.ORDER)), "a test is listed twice in ORDER")
        self.assertEqual(sorted(ctutest.ORDER), sorted(names), "ORDER and the t_* functions differ")

    def test_every_test_has_a_description(self):
        for f in ctutest.TESTS:
            self.assertTrue(f.__doc__ and f.__doc__.strip(), f.__name__)


if __name__ == "__main__":
    unittest.main()
