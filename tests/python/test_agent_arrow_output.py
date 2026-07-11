"""Verify blazerules_agent can write decisions as an Arrow IPC binary stream."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[2]
_BUILDS = ("cmake-build-release", "build-python-smoke2", "build-python-smoke", "build")


def _find_agent() -> str | None:
    env = os.environ.get("BLAZERULES_AGENT")
    if env and Path(env).exists():
        return env
    on_path = shutil.which("blazerules_agent")
    if on_path:
        return on_path
    for build in _BUILDS:
        cand = _ROOT / build / "blazerules_agent"
        if cand.exists():
            return str(cand)
    return None


_AGENT = _find_agent()

try:
    import pyarrow.ipc as ipc
    _HAVE_PYARROW = True
except Exception:
    _HAVE_PYARROW = False

RULES = {
    "schema_version": "2.1",
    "fields": {"amount": {"type": "float32", "nullable": False}},
    "ruleset": {
        "name": "arrow-out",
        "version": "1.0.0",
        "rules": [
            {
                "id": "high_amount",
                "action": "flag",
                "severity": "HIGH",
                "conditions": {"field": "amount", "op": "gt", "value": 100},
            }
        ],
    },
}

RECORDS = [{"amount": 50}, {"amount": 150}, {"amount": 200}]


@unittest.skipUnless(_AGENT, "blazerules_agent binary not found (set BLAZERULES_AGENT)")
@unittest.skipUnless(_HAVE_PYARROW, "pyarrow not available")
class AgentArrowOutputTest(unittest.TestCase):
    def test_arrow_output_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            rules_path = Path(d) / "rules.yaml"
            rules_path.write_text(json.dumps(RULES))
            out_path = Path(d) / "decisions.arrow"
            ndjson = ("\n".join(json.dumps(r) for r in RECORDS) + "\n").encode()
            subprocess.run(
                [_AGENT, "--rules", str(rules_path), "--input", "stdin",
                 "--output", "arrow", "--output-path", str(out_path)],
                input=ndjson, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            table = ipc.open_stream(str(out_path)).read_all()
            names = [f.name for f in table.schema]
            for col in ("ts_ms", "instance", "batch_row", "ruleset_version",
                        "matched", "decision", "score", "risk_band", "winning_rule_id"):
                self.assertIn(col, names)
            data = table.to_pydict()
            self.assertEqual(table.num_rows, len(RECORDS))
            self.assertEqual(data["decision"], ["APPROVE", "FLAG", "FLAG"])
            self.assertEqual(data["winning_rule_id"], ["", "high_amount", "high_amount"])

    def test_output_none_suppresses_row_stream(self):
        with tempfile.TemporaryDirectory() as d:
            rules_path = Path(d) / "rules.yaml"
            rules_path.write_text(json.dumps(RULES))
            ndjson = ("\n".join(json.dumps(r) for r in RECORDS) + "\n").encode()
            completed = subprocess.run(
                [_AGENT, "--rules", str(rules_path), "--input", "stdin", "--output", "none"],
                input=ndjson, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            self.assertEqual(completed.stdout, b"")


if __name__ == "__main__":
    unittest.main()
