"""Python-level tests for output_detail semantics and fail-closed rule loading.

Self-contained (inline rules, no external fixtures) so it runs anywhere the compiled
``blazerules`` module is importable. Run with:

    PYTHONPATH=cmake-build-release python3 -m unittest \
        tests.python.test_output_detail_and_failclosed -v

or let the sys.path bootstrap below discover a local build directory.
"""
from __future__ import annotations

import json
import sys
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

import blazerules  # noqa: E402


RULES = json.dumps(
    {
        "ruleset": {
            "name": "output-detail-tests",
            "version": "1.0.0",
            "rules": [
                {
                    "id": "big_amount",
                    "action": "review",
                    "severity": "HIGH",
                    "conditions": {"field": "amount", "op": "gt", "value": 100},
                },
                {
                    "id": "blocked_country",
                    "action": "block",
                    "severity": "CRITICAL",
                    "conditions": {"field": "country", "op": "eq", "value": "CN"},
                },
            ],
        }
    }
)

RECORDS = [
    json.dumps({"amount": 250, "country": "US"}),   # big_amount -> REVIEW
    json.dumps({"amount": 10, "country": "CN"}),    # blocked_country -> BLOCK
    json.dumps({"amount": 5, "country": "US"}),     # no match -> APPROVE
]


def _engine(detail):
    config = blazerules.EngineConfig()
    config.output_detail = detail
    eng = blazerules.RuleEngine(config)
    eng.load_rules_from_string(RULES)
    return eng


class OutputDetailParityTest(unittest.TestCase):
    """Routing outputs must be identical under DECISIONS and BITMASKS."""

    def test_routing_outputs_match_across_modes(self):
        dec = _engine(blazerules.OutputDetail.DECISIONS).evaluate_messages(RECORDS)
        bit = _engine(blazerules.OutputDetail.BITMASKS).evaluate_messages(RECORDS)

        self.assertEqual(list(dec.decisions), list(bit.decisions))
        self.assertEqual(list(dec.scores), list(bit.scores))
        self.assertEqual(list(dec.winning_rule_ids), list(bit.winning_rule_ids))
        self.assertEqual(dict(dec.match_counts), dict(bit.match_counts))
        self.assertEqual(dec.n_matched, bit.n_matched)
        # match_counts is available in BOTH modes.
        self.assertEqual(dec.match_counts.get("big_amount"), 1)
        self.assertEqual(dec.match_counts.get("blocked_country"), 1)

    def test_bitmasks_expose_per_rule_masks(self):
        result = _engine(blazerules.OutputDetail.BITMASKS).evaluate_messages(RECORDS)
        mask = result["big_amount"]
        self.assertEqual(len(mask), len(RECORDS))
        self.assertTrue(bool(mask[0]))          # row 0 matched big_amount
        self.assertFalse(bool(mask[1]))         # row 1 did not
        rows = list(result.indices_for_rule("big_amount"))
        self.assertEqual(rows, [0])

    def test_decisions_mode_has_no_per_rule_masks(self):
        result = _engine(blazerules.OutputDetail.DECISIONS).evaluate_messages(RECORDS)
        # Per-rule attribution is not materialized under DECISIONS.
        with self.assertRaises(KeyError):
            _ = result["big_amount"]
        with self.assertRaises(KeyError):
            result.indices_for_rule("big_amount")

    def test_counts_mode_keeps_only_batch_counts(self):
        result = _engine(blazerules.OutputDetail.COUNTS).evaluate_messages(RECORDS)

        self.assertEqual(result.n_records, len(RECORDS))
        self.assertEqual(result.n_matched, 2)
        self.assertEqual(result.match_counts.get("big_amount"), 1)
        self.assertEqual(result.match_counts.get("blocked_country"), 1)
        self.assertEqual(len(result.matched_indices), 0)
        self.assertEqual(list(result.decisions), [])
        self.assertEqual(len(result.decision_codes), 0)
        self.assertEqual(list(result.scores), [])
        self.assertEqual(list(result.risk_bands), [])
        self.assertEqual(list(result.winning_rule_ids), [])
        self.assertEqual(result.grouped_decision_indices(), {})
        self.assertEqual(result.model_scores, {})
        with self.assertRaises(KeyError):
            _ = result["big_amount"]

    def test_codes_mode_materializes_codes_only(self):
        result = _engine(blazerules.OutputDetail.CODES).evaluate_messages(RECORDS)

        labels = result.decision_label_map
        decisions = [labels[int(code)] for code in result.decision_codes]
        self.assertEqual(decisions, ["REVIEW", "BLOCK", "APPROVE"])
        self.assertEqual(result.n_matched, 2)
        self.assertEqual(result.match_counts.get("big_amount"), 1)
        self.assertEqual(result.match_counts.get("blocked_country"), 1)
        self.assertEqual(len(result.matched_indices), 0)
        self.assertEqual(list(result.decisions), [])
        self.assertEqual(list(result.scores), [])
        self.assertEqual(list(result.risk_bands), [])
        self.assertEqual(list(result.winning_rule_ids), [])
        self.assertEqual(result.grouped_decision_indices(), {})

    def test_json_array_direct_path_matches_ndjson(self):
        array_payload = ("[" + ",".join(RECORDS) + "]").encode()
        ndjson_payload = ("\n".join(RECORDS) + "\n").encode()

        array_result = _engine(blazerules.OutputDetail.DECISIONS).evaluate_json_array(array_payload)
        ndjson_result = _engine(blazerules.OutputDetail.DECISIONS).evaluate_ndjson(ndjson_payload)

        self.assertEqual(array_result.n_records, ndjson_result.n_records)
        self.assertEqual(array_result.n_matched, ndjson_result.n_matched)
        self.assertEqual(list(array_result.decisions), list(ndjson_result.decisions))
        self.assertEqual(dict(array_result.match_counts), dict(ndjson_result.match_counts))

    def test_padded_json_array_uses_logical_size(self):
        logical = ("[" + ",".join(RECORDS) + "]").encode()
        padded = logical + (b"\0" * 128)

        result = _engine(blazerules.OutputDetail.CODES).evaluate_json_array_padded(
            padded, len(logical)
        )

        labels = result.decision_label_map
        decisions = [labels[int(code)] for code in result.decision_codes]
        self.assertEqual(decisions, ["REVIEW", "BLOCK", "APPROVE"])
        self.assertEqual(result.n_matched, 2)


class FailClosedLoadTest(unittest.TestCase):
    """Malformed rules must raise, not silently degrade."""

    def _load(self, rule_body):
        rules = json.dumps(
            {"ruleset": {"name": "fc", "version": "1.0.0", "rules": [rule_body]}}
        )
        blazerules.RuleEngine().load_rules_from_string(rules)

    def test_unknown_action_rejected(self):
        with self.assertRaises(blazerules.BlazeRulesError):
            self._load(
                {
                    "id": "r",
                    "action": "blok",  # typo for "block"
                    "conditions": {"field": "amount", "op": "gt", "value": 1},
                }
            )

    def test_unknown_operator_rejected(self):
        with self.assertRaises(blazerules.BlazeRulesError):
            self._load(
                {
                    "id": "r",
                    "action": "flag",
                    "conditions": {"field": "amount", "op": "definitely_not_an_op", "value": 1},
                }
            )

    def test_malformed_nested_condition_rejected(self):
        with self.assertRaises(blazerules.BlazeRulesError):
            self._load(
                {
                    "id": "r",
                    "action": "flag",
                    "conditions": {
                        "and": [
                            {"field": "amount", "op": "gt", "value": 1},
                            {"field": "amount", "op": "not_a_real_op", "value": 2},
                        ]
                    },
                }
            )

    def test_deeply_nested_condition_rejected(self):
        # A pathologically deep condition tree must be rejected by the depth guard,
        # not overflow the stack (mirrors the C++ RejectsDeeplyNested* tests).
        cond = {"field": "amount", "op": "gt", "value": 1}
        for _ in range(300):
            cond = {"and": [cond]}
        with self.assertRaises(blazerules.BlazeRulesError):
            self._load({"id": "r", "action": "flag", "conditions": cond})

    def test_valid_rule_still_loads(self):
        # Sanity: strictness must not reject legitimate rules.
        self._load(
            {
                "id": "r",
                "action": "review",
                "severity": "HIGH",
                "conditions": {"field": "amount", "op": "gt", "value": 1},
            }
        )


if __name__ == "__main__":
    unittest.main()
