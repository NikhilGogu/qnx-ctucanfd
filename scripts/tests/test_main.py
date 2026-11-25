# Copyright 2026 The ctucanfd authors
# SPDX-License-Identifier: Apache-2.0
"""Unit tests for the case selection, the summary line and the exit status of
ctutest.py and signature.py. No target is needed: the target, the bus and the
cases are replaced by stand-ins. Run on any host:

    python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""
import contextlib
import io
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctutest  # noqa: E402
import signature  # noqa: E402


class Dummy:
    """Stands in for Target and Bus."""
    deployed = {}

    def __init__(self, *args):
        pass

    def stop_all(self):
        pass


class MainTest(unittest.TestCase):
    def setUp(self):
        self.calls = []

        def t_alpha(t, bus):
            self.calls.append("alpha")
            return "ok"

        def t_beta(t, bus):
            self.calls.append("beta")
            return "ok"

        def t_gamma(t, bus):
            self.calls.append("gamma")
            raise ctutest.Skip("needs a peer")

        def t_delta(t, bus):
            self.calls.append("delta")
            raise ctutest.Fail("broken")

        def t_epsilon(t, bus):
            self.calls.append("epsilon")
            raise ctutest.Skip("needs something else")

        self.funcs = [t_alpha, t_beta, t_gamma, t_delta, t_epsilon]

    def run_main(self, args, funcs=None):
        out = io.StringIO()
        with mock.patch.object(ctutest, "TESTS", funcs or self.funcs), \
                mock.patch.object(ctutest, "ORDER", []), \
                mock.patch.object(ctutest, "Target", Dummy), mock.patch.object(ctutest, "Bus", Dummy), \
                mock.patch.object(sys, "argv", ["ctutest.py"] + args), contextlib.redirect_stdout(out):
            rc = ctutest.main()
        return rc, out.getvalue().splitlines()

    def test_full_run_summary_is_unchanged(self):
        rc, lines = self.run_main(["alpha", "beta"])
        self.assertEqual(rc, 0)
        self.assertEqual(lines[-1], "2 of 2 passed")

    def test_skip_is_in_the_summary_and_fails_the_run(self):
        rc, lines = self.run_main(["alpha", "gamma"])
        self.assertEqual(lines[-1], "1 of 1 passed, 1 skipped (gamma)")
        self.assertEqual(rc, 1)

    def test_only_a_skip_is_not_a_pass(self):
        rc, lines = self.run_main(["gamma"])
        self.assertEqual(lines[-1], "0 of 0 passed, 1 skipped (gamma)")
        self.assertEqual(rc, 1)

    def test_skip_on_purpose_keeps_exit_zero(self):
        rc, lines = self.run_main(["--allow-skip", "gamma", "alpha", "gamma"])
        self.assertEqual(lines[-1], "1 of 1 passed, 1 skipped (gamma)")
        self.assertEqual(rc, 0)

    def test_a_second_skipped_case_still_fails_the_run(self):
        rc, lines = self.run_main(["--allow-skip", "gamma", "alpha", "gamma", "epsilon"])
        self.assertEqual(lines[-1], "1 of 1 passed, 2 skipped (gamma, epsilon)")
        self.assertEqual(rc, 1)

    def test_allow_skip_takes_a_list(self):
        rc, lines = self.run_main(["--allow-skip", "gamma", "--allow-skip", "epsilon", "alpha", "gamma", "epsilon"])
        self.assertEqual(rc, 0)

    def test_allowing_a_case_that_did_not_skip_changes_nothing(self):
        rc, lines = self.run_main(["--allow-skip", "alpha", "gamma"])
        self.assertEqual(rc, 1)

    def test_allow_skip_does_not_hide_a_failure(self):
        rc, lines = self.run_main(["--allow-skip", "gamma", "alpha", "gamma", "delta"])
        self.assertEqual(lines[-1], "1 of 2 passed, 1 skipped (gamma)")
        self.assertEqual(rc, 1)

    def test_case_not_named_is_not_a_skip(self):
        rc, lines = self.run_main(["alpha", "beta"])
        self.assertNotIn("gamma", self.calls)
        self.assertEqual(rc, 0)

    def test_a_case_named_twice_runs_and_counts_once(self):
        rc, lines = self.run_main(["alpha", "alpha", "beta", "alpha"])
        self.assertEqual(self.calls, ["alpha", "beta"])
        self.assertEqual(lines[-1], "2 of 2 passed")
        self.assertEqual(rc, 0)

    def test_duplicates_in_the_junit_file(self):
        path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "ctutest-main-test.xml")
        try:
            self.run_main(["--junit", path, "alpha", "alpha"])
            with open(path) as f:
                self.assertEqual(f.read().count("<testcase "), 1)
        finally:
            if os.path.exists(path):
                os.remove(path)


class SignatureMainTest(unittest.TestCase):
    def test_a_case_named_twice_runs_once(self):
        calls = []
        tests = {"one": lambda t, bus: calls.append("one") or "ok", "two": lambda t, bus: "ok"}
        out = io.StringIO()
        with mock.patch.object(signature, "TESTS", tests), mock.patch.object(signature.c, "Target", Dummy), \
                mock.patch.object(signature.c, "Bus", Dummy), \
                mock.patch.object(sys, "argv", ["signature.py", "one", "one"]), contextlib.redirect_stdout(out):
            rc = signature.main()
        self.assertEqual(calls, ["one"])
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
