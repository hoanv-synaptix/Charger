"""Host-only checks for strict preflight and report behavior."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from .runner import run
from .transports import DryRunBackend


class NotReadyBackend(DryRunBackend):
    def __init__(self):
        super().__init__(ready=False)


class RunnerTests(unittest.TestCase):
    def test_dry_run_produces_pass_report(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(run(DryRunBackend(), directory), 0)
            self.assertEqual(len(list(Path(directory).glob("*/summary.json"))), 1)

    def test_preflight_failure_is_not_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertNotEqual(run(NotReadyBackend(), directory), 0)
            report = next(Path(directory).glob("*/summary.json"))
            self.assertIn('"BLOCKED"', report.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
