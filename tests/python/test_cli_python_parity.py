"""CLI/Python parity checks for the installed or locally built package."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# Bootstrap: make a locally built extension importable without installing.
_ROOT = Path(__file__).resolve().parents[2]
_LOCAL_BUILDS = ("build-python-smoke2", "build-python-smoke", "cmake-build-release",
                 "cmake-build-debug", "build")
for _build in reversed(_LOCAL_BUILDS):
    _p = _ROOT / _build
    if _p.exists():
        sys.path.insert(0, str(_p))

try:
    import blazerules  # noqa: E402
    _HAVE_MODULE = True
except Exception:  # pragma: no cover - environment dependent
    _HAVE_MODULE = False


def _find_cli() -> str | None:
    env = os.environ.get("BLAZERULES_CLI")
    if env and Path(env).exists():
        return env
    on_path = shutil.which("blazerules")
    if on_path:
        return on_path
    for build in _LOCAL_BUILDS:
        cand = _ROOT / build / "blazerules"
        if cand.exists():
            return str(cand)
    return None


_CLI = _find_cli()

RULES = {
    "schema_version": "2.1",
    "fields": {
        "amount": {"type": "float32", "nullable": False},
        "country": {"type": "categorical", "nullable": False},
    },
    "ruleset": {
        "name": "parity",
        "version": "1.0.0",
        "rules": [
            {
                "id": "high_amount",
                "action": "flag",
                "severity": "HIGH",
                "conditions": {"field": "amount", "op": "gt", "value": 100},
            },
            {
                "id": "blocked_country",
                "action": "block",
                "severity": "CRITICAL",
                "conditions": {"field": "country", "op": "eq", "value": "XX"},
            },
        ],
    },
}

RECORDS = [
    {"amount": 50, "country": "US"},    # no match -> APPROVE
    {"amount": 150, "country": "US"},   # high_amount -> FLAG
    {"amount": 200, "country": "XX"},   # both -> BLOCK wins
    {"amount": 10, "country": "XX"},    # blocked_country -> BLOCK
]


@unittest.skipUnless(_HAVE_MODULE, "blazerules Python module not importable")
@unittest.skipUnless(_CLI, "blazerules CLI binary not found (set BLAZERULES_CLI)")
class CliPythonParityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        cls.rules_path = d / "rules.yaml"
        cls.rules_path.write_text(json.dumps(RULES))  # YAML is a superset of JSON
        cls.ndjson_path = d / "events.ndjson"
        cls.ndjson_path.write_text("\n".join(json.dumps(r) for r in RECORDS) + "\n")
        cls.jsonarray_path = d / "events.json"
        cls.jsonarray_path.write_text(json.dumps(RECORDS))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def _cli(self, *args: str) -> str:
        out = subprocess.run(
            [_CLI, *args], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        return out.stdout.decode()

    def _python_rows(self):
        eng = blazerules.RuleEngine(blazerules.EngineConfig())
        eng.load_rules(str(self.rules_path))
        ndjson = self.ndjson_path.read_bytes()
        res = eng.evaluate_ndjson(ndjson)
        matched = set(int(i) for i in res.matched_indices)
        rows = []
        for i in range(res.n_records):
            rows.append(
                {
                    "row": i,
                    "matched": i in matched,
                    "decision": list(res.decisions)[i],
                    "score": float(list(res.scores)[i]),
                    "risk_band": list(res.risk_bands)[i],
                    "winning_rule_id": list(res.winning_rule_ids)[i],
                }
            )
        return res, rows

    # ---- per-row decisions parity (NDJSON) ----
    def test_decisions_jsonl_matches_python(self):
        _res, py_rows = self._python_rows()
        cli_out = self._cli(
            "eval", "--rules", str(self.rules_path),
            "--input", "ndjson", "--path", str(self.ndjson_path),
            "--output", "decisions-jsonl",
        )
        cli_rows = [json.loads(line) for line in cli_out.splitlines() if line.strip()]
        self.assertEqual(len(cli_rows), len(py_rows))
        for py_r, cli_r in zip(py_rows, cli_rows):
            self.assertEqual(cli_r["decision"], py_r["decision"])
            self.assertEqual(cli_r["matched"], py_r["matched"])
            self.assertAlmostEqual(cli_r["score"], py_r["score"], places=4)
            self.assertEqual(cli_r["risk_band"], py_r["risk_band"])
            self.assertEqual(cli_r["winning_rule_id"], py_r["winning_rule_id"])

    # ---- grouped-decisions parity ----
    def test_grouped_decisions_matches_python(self):
        res, _rows = self._python_rows()
        py_grouped = {
            k: sorted(int(i) for i in v)
            for k, v in res.grouped_decision_indices().items()
        }
        cli_grouped = json.loads(
            self._cli(
                "eval", "--rules", str(self.rules_path),
                "--input", "ndjson", "--path", str(self.ndjson_path),
                "--output", "grouped-decisions",
            )
        )
        cli_grouped = {k: sorted(v) for k, v in cli_grouped.items()}
        self.assertEqual(cli_grouped, py_grouped)

    # ---- rule-counts parity ----
    def test_rule_counts_matches_python(self):
        res, _rows = self._python_rows()
        py_counts = {k: int(v) for k, v in dict(res.match_counts).items()}
        cli_counts = json.loads(
            self._cli(
                "eval", "--rules", str(self.rules_path),
                "--input", "ndjson", "--path", str(self.ndjson_path),
                "--output", "rule-counts",
            )
        )
        self.assertEqual({k: int(v) for k, v in cli_counts.items()}, py_counts)

    # ---- json-array input matches ndjson input ----
    def test_json_array_input_matches_ndjson(self):
        ndjson_out = self._cli(
            "eval", "--rules", str(self.rules_path),
            "--input", "ndjson", "--path", str(self.ndjson_path),
            "--output", "grouped-decisions",
        )
        jsonarray_out = self._cli(
            "eval", "--rules", str(self.rules_path),
            "--input", "json-array", "--path", str(self.jsonarray_path),
            "--output", "grouped-decisions",
        )
        self.assertEqual(json.loads(ndjson_out), json.loads(jsonarray_out))

    # ---- arrow-ipc output round-trips and matches Python decisions ----
    def test_arrow_ipc_output_matches_python(self):
        try:
            import pyarrow.ipc as ipc
        except Exception:
            self.skipTest("pyarrow not available to read arrow-ipc output")
        _res, py_rows = self._python_rows()
        out_path = Path(self.tmp.name) / "decisions.arrow"
        self._cli(
            "eval", "--rules", str(self.rules_path),
            "--input", "ndjson", "--path", str(self.ndjson_path),
            "--output", "arrow-ipc", "--output-path", str(out_path),
        )
        table = ipc.open_stream(str(out_path)).read_all().to_pydict()
        self.assertEqual(list(table["decision"]), [r["decision"] for r in py_rows])
        self.assertEqual(list(table["matched"]), [r["matched"] for r in py_rows])
        self.assertEqual(
            [round(s, 4) for s in table["score"]],
            [round(r["score"], 4) for r in py_rows],
        )
        self.assertEqual(list(table["winning_rule_id"]), [r["winning_rule_id"] for r in py_rows])

    # ---- info reports capabilities as JSON ----
    def test_info_reports_capabilities(self):
        info = json.loads(self._cli("info"))
        self.assertIn("version", info)
        self.assertIn("simd_backend", info)
        self.assertIn("features", info)
        for feature in ("onnx", "kafka", "avro", "protobuf", "s3"):
            self.assertIn(feature, info["features"])

    # ---- unified --config produces the same result as explicit flags ----
    def test_config_file_matches_flags(self):
        cfg_path = Path(self.tmp.name) / "run.yaml"
        cfg_path.write_text(
            json.dumps(
                {
                    "rules": str(self.rules_path),
                    "input": {"type": "ndjson", "path": str(self.ndjson_path)},
                    "output": {"mode": "rule-counts"},
                }
            )
        )
        via_config = self._cli("eval", "--config", str(cfg_path))
        via_flags = self._cli(
            "eval", "--rules", str(self.rules_path),
            "--input", "ndjson", "--path", str(self.ndjson_path),
            "--output", "rule-counts",
        )
        self.assertEqual(json.loads(via_config), json.loads(via_flags))

    # ---- malformed input: both surfaces skip the bad row, keep the good ones ----
    def test_malformed_input_parity(self):
        bad = Path(self.tmp.name) / "bad.ndjson"
        bad.write_text(
            json.dumps({"amount": 150, "country": "US"}) + "\n"
            + "{not valid json\n"
            + json.dumps({"amount": 200, "country": "XX"}) + "\n"
        )
        eng = blazerules.RuleEngine(blazerules.EngineConfig())
        eng.load_rules(str(self.rules_path))
        py_res = eng.evaluate_ndjson(bad.read_bytes())
        cli_summary = json.loads(
            self._cli(
                "eval", "--rules", str(self.rules_path),
                "--input", "ndjson", "--path", str(bad), "--output", "summary",
            )
        )
        self.assertEqual(cli_summary["records"], py_res.n_records)
        self.assertEqual(cli_summary["messages_skipped"], py_res.messages_skipped)


if __name__ == "__main__":
    unittest.main()
