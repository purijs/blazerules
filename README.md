# BlazeRules

[![Docs](https://img.shields.io/badge/docs-readme.io-2563eb)](https://blazerules.readme.io/docs/getting-started)
[![GitHub](https://img.shields.io/badge/github-purijs%2Fblazerules-111827?logo=github)](https://github.com/purijs/blazerules)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)
![Python](https://img.shields.io/badge/python-pybind11-3776AB?logo=python)
![Build](https://img.shields.io/badge/build-Release-success)

BlazeRules is an embeddable C++20 vectorized decision engine with a Python
module named `blazerules`. It evaluates YAML rule sets over batches of JSON,
NDJSON, or Apache Arrow records. The core is batch-first: collect records from a
queue, file, API, or in-memory producer, then call the engine once per batch.

Repository: [github.com/purijs/blazerules](https://github.com/purijs/blazerules) ·
Documentation: [blazerules.readme.io](https://blazerules.readme.io/docs/getting-started)

The project is a library, not a hosted service. It is designed for fraud/risk
screening, compliance routing, eligibility checks, AdTech filtering, and offline
backtesting where deterministic rules should run before more expensive systems.

## What It Does

- Compiles YAML rules once into immutable execution plans.
- Runs columnar predicates over Arrow buffers and transposed JSON batches.
- Infers schema from the first evaluated batch when users do not provide one.
- Supports flat and nested records through dotted field names.
- Supports `array_any` / SQL `any_match(...)` for arrays of objects.
- Reuses shared predicates globally and evaluates common scans once.
- Emits decisions, scores, risk bands, winning rules, match counts, and optional
  per-rule bitmasks.
- Supports windows, lookups, regex, CIDR, temporal, geo, vector distance, and
  ONNX `model_score` rules.
- Uses runtime-dispatched SIMD kernels: ARM64 NEON, x86_64 AVX2/FMA, optional
  AVX-512, and scalar fallback.
- Exposes `blazerules_io` connectors/decoders for Kafka, CDC, Arrow IPC,
  Avro, Protobuf, local files, and exact-object `s3://` reads.
- Includes the local read-only dashboard and the multi-input agent in full
  release builds.

## Repository Layout

```text
include/blazerules/      C++ public core headers
include/blazerules_io/   IO/streaming headers
src/core/                Engine, kernels, transposer, dictionaries, windows
src/compiler/            YAML/SQL parser, compiler, validation, conflicts
src/bindings/            pybind11 modules
src/io/                  Kafka/file/decoder implementation
src/dashboard/           Local read-only dashboard
src/agent/               Local multi-input agent
charts/                  Optional Helm chart
rules.yaml               Complete compact rule and multi-instance sample
sample_transaction.json  JSON record matching rules.yaml
sample_lookups/          Small lookup CSVs used by rules.yaml
```

Generated build directories, virtualenvs, benchmark results, stress corpora,
models, and lookup data are intentionally ignored by git.

## Build

## Install From PyPI

For the full Python package:

```bash
pip install blazerules
```

The release wheel is built full-feature: native `blazerules`, `blazerules_io`,
ONNX `model_score`, Kafka/CDC/Arrow IPC/Avro/Protobuf/S3 IO, dashboard, agent,
runtime-dispatched SIMD kernels, schema inference, windows, lookups, regex,
CIDR, temporal, geo, vector similarity, and decisions/scoring. `numpy` and
`pyarrow` are declared Python runtime dependencies and are installed by pip.

```bash
python -c "import blazerules, blazerules_io; print(blazerules.__version__, blazerules.simd_backend())"
```

## Build From Source

macOS arm64 prerequisites:

```bash
brew install cmake ninja autoconf autoconf-archive automake libtool
```

Configure and build the core library, Python module, and driver:

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/.vcpkg-clion/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -G Ninja

cmake --build cmake-build-release --target blazerules_core blazerules blazerules_driver -j
```

Smoke:

```bash
export PYTHONPATH="$PWD/cmake-build-release"
python -c "import blazerules, blazerules_io; print(blazerules.__version__, blazerules.simd_backend())"
./cmake-build-release/blazerules_driver rules.yaml
```

CMake presets are included for common production shapes:

```bash
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release -j

cmake --preset linux-x86_64-release-dispatch
cmake --build --preset linux-x86_64-release-dispatch -j

cmake --preset cloud-portable-release
cmake --build --preset cloud-portable-release -j

cmake --preset windows-x64-release-dispatch
cmake --build --preset windows-x64-release-dispatch -j
```

## CMake Options

| Option | Default | Purpose |
| --- | --- | --- |
| `BLAZERULES_ENABLE_ONNX` | `ON` | Enables `model_score` rules and `register_model()` |
| `BLAZERULES_IO` | `ON` | Builds `blazerules_io` connectors/decoders |
| `BLAZERULES_IO_KAFKA` | `ON` | Kafka source/sink inside `blazerules_io` |
| `BLAZERULES_IO_AVRO` | `ON` | Avro binary decoder |
| `BLAZERULES_IO_PROTOBUF` | `ON` | Protobuf descriptor decoder |
| `BLAZERULES_DASHBOARD` | `ON` | Local read-only dashboard executable |
| `BLAZERULES_AGENT` | `ON` | Local multi-input log/HTTP/file agent |
| `BLAZERULES_NATIVE_TUNE` | `ON` | Local `-march=native` style tuning |
| `BLAZERULES_X86_AVX2` | `ON` | Builds runtime-dispatched AVX2 kernels on x86_64 |
| `BLAZERULES_X86_AVX512` | `ON` | Builds optional AVX-512 kernels on x86_64 |

The PyPI release workflow builds with the full feature set enabled. For a local
source build, the defaults are also full-feature, but the flags can still be
spelled out explicitly:

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBLAZERULES_ENABLE_ONNX=ON \
  -DBLAZERULES_IO=ON \
  -DBLAZERULES_IO_KAFKA=ON \
  -DBLAZERULES_IO_AVRO=ON \
  -DBLAZERULES_IO_PROTOBUF=ON \
  -DBLAZERULES_IO_S3=ON \
  -DBLAZERULES_DASHBOARD=ON \
  -DBLAZERULES_AGENT=ON \
  -DBLAZERULES_NATIVE_TUNE=ON \
  -DBLAZERULES_X86_AVX2=ON \
  -DBLAZERULES_X86_AVX512=ON \
  -G Ninja
```

## Dashboard Preview

The dashboard is a local, read-only operational UI for rules, decision
logs, dead-letter logs, source health, and benchmark summaries.

![BlazeRules dashboard overview](https://raw.githubusercontent.com/purijs/blazerules/main/assets/dashboard-overview.png)

## Python Quick Start

```python
import blazerules

config = blazerules.EngineConfig()
config.output_detail = blazerules.OutputDetail.DECISIONS

engine = blazerules.RuleEngine(config)
engine.load_rules("rules.yaml")

payload = b"""
{"card_token":"card_1","amount":2500.0,"device_type":"emulator",
 "country_code":"US","account_age_days":2,"hour_of_day":1.5}
{"card_token":"card_2","amount":50.0,"device_type":"ios",
 "country_code":"GB","account_age_days":400,"hour_of_day":12}
"""

result = engine.evaluate_ndjson(payload)
print(result.n_records, result.n_matched)
print(result.decisions)
print(result.match_counts)
```

Rules can be loaded before a schema exists. The first evaluated batch samples
rule-referenced fields, infers supported types, binds the schema, then compiles
and activates the loaded rules. If you want explicit control, construct
`RuleEngine(schema, config)` with `blazerules.Field(...)` definitions.

## Arrow Evaluation

Use Arrow when upstream data is already typed or when JSON parsing is not what
you want to measure.

```python
import pyarrow as pa
import blazerules

batch = pa.record_batch({
    "card_token": pa.array(["card_1", "card_2"]),
    "amount": pa.array([2500.0, 50.0], type=pa.float32()),
    "device_type": pa.array(["emulator", "ios"]),
    "country_code": pa.array(["US", "GB"]),
    "account_age_days": pa.array([2, 400], type=pa.int32()),
    "hour_of_day": pa.array([1.5, 12.0], type=pa.float32()),
})

engine = blazerules.RuleEngine()
engine.load_rules("rules.yaml")
result = engine.evaluate_batch(batch)
```

Arrow batches may contain extra columns or different physical column order.
BlazeRules projects rule-referenced columns by name. Nested Arrow `struct`
fields use the same dotted names as JSON.

## YAML Rule Format

Minimal shape:

```yaml
schema_version: "2.1"

fields:
  card_token: {type: entity_key, nullable: false}
  amount: {type: float32, nullable: false}
  device_type:
    type: categorical
    values: [ios, android, web, emulator]

ruleset:
  name: Fraud Rules
  version: "1.0.0"
  rules:
    - id: high_amount_emulator
      action: block
      severity: HIGH
      weight: 40
      conditions:
        and:
          - field: amount
            op: gt
            value: 2000
          - field: device_type
            op: eq
            value: emulator
```

Top-level `fields` are optional hints, not a mandatory user schema. They are
useful for entity keys, timestamps, nullability, and closed categorical values.
Without hints, BlazeRules infers referenced fields from the first batch.

Logical forms:

```yaml
conditions:
  and:
    - field: amount
      op: gt
      value: 1000
    - or:
        - field: country_code
          op: in
          values: [US, GB]
        - not:
            field: device_type
            op: eq
            value: ios
```

SQL expression form:

```yaml
conditions:
  sql: "amount > 1000 AND any_match(items, x -> x.price > 100)"
```

See `rules.yaml` for a compact file covering every operator family supported
by the parser, plus a top-level `instances` section for the local agent.

## Operator Summary

Numeric:

```text
gt lt gte lte eq neq
between_including between_excluding
gt_field lt_field gte_field lte_field eq_field neq_field
```

Categorical/entity:

```text
eq neq in not_in
```

Null and empty:

```text
is_null is_not_null is_empty is_not_empty
```

Strings and regex:

```text
contains starts_with ends_with ci_eq
length_gt length_lt length_eq
regex not_regex
```

Arrays and flags:

```text
contains_any contains_all intersects not_intersects
array_len_gt array_len_lt array_len_eq
flags_any flags_all flags_none
array_any
```

Network, temporal, geo:

```text
ip_in_subnet ip_not_in_subnet
before after within_last day_of_week_in time_of_day_between
distance_gt distance_lt
```

Lookups, windows, derived values:

```text
in_lookup not_in_lookup
window: count sum avg ratio min max
expr arithmetic: + - * /
vector_distance: cosine l2 dot
model_score
```

## Nested Records And Arrays Of Objects

Nested JSON:

```json
{"merchant":{"risk":{"score":91}}}
```

Rule:

```yaml
conditions:
  field: merchant.risk.score
  op: gt
  value: 50
```

Array-of-object same-element semantics:

```yaml
conditions:
  array_any:
    path: items
    where:
      and:
        - field: price
          op: gt
          value: 100
        - field: category
          op: eq
          value: electronics
```

This matches only when one item has both `price > 100` and
`category == electronics`.

## Lookups

Rule files can reference CSV lookup sets:

```yaml
lookups:
  blocked_merchants:
    type: string_set
    path: lookups/blocked_merchants.csv
  risky_bins:
    type: int_set
    path: lookups/risky_bins.csv
  vpn_ranges:
    type: ipv4_cidr_set
    path: lookups/vpn_ranges.csv
```

Supported lookup CSV columns:

| Type | Column |
| --- | --- |
| `string_set` | `value` |
| `int_set` | `value` |
| `ipv4_cidr_set` | `cidr` |

Relative lookup paths resolve relative to the rules file. Missing or invalid
lookup files fail rule loading and do not replace an active hot-reloaded ruleset.

## Decisions And Routing

Use decision groups instead of Python loops over every row:

```python
result = engine.evaluate_ndjson(payload)

approved = result.indices_for_decision("APPROVE")
needs_review = result.indices_for_not_decision("APPROVE")
groups = result.grouped_decision_indices()
```

Useful result fields:

```text
n_records
n_matched
decisions
decision_codes
scores
risk_bands
winning_rule_ids
match_counts
matched_indices
timing_ms
messages_processed
messages_skipped
error_counts
error_samples
```

Use `OutputDetail.DECISIONS` for routing and `OutputDetail.BITMASKS` only when
downstream code needs per-rule bitmasks.

## Windows

Window rules read prior batch history, inject derived window columns, evaluate
the current batch, then write the current batch for future batches. This means
batch N sees state committed by earlier batches. Same-batch repeated entity rows
do not see earlier rows from that same batch by default.

Supported window functions:

```text
count sum avg ratio min max
```

## Hot Reload

```python
engine.load_rules("rules.yaml")
engine.enable_hot_reload("rules.yaml", poll_interval_seconds=5)
status = engine.hot_reload_status()
```

Reload compiles and validates the new YAML/lookups off the hot path, then swaps
atomically only on success. Failed reloads keep the previous ruleset active.
Batches keep the ruleset observed at batch start.

## Error Handling

Rules and schema activation are strict. Bad YAML, unknown fields, duplicate rule
IDs, invalid regex, bad lookup files, and type/operator mismatches fail before
activation.

Ingest defaults are tolerant:

```python
config.ingest_error_mode = blazerules.IngestErrorMode.SKIP_AND_COUNT
config.type_mismatch_mode = blazerules.TypeMismatchMode.NULL_ON_TYPE_ERROR
```

Other modes:

```text
SKIP_TO_DEAD_LETTER
HARD_FAIL
COERCE
HARD_FAIL_TYPE
```

## SIMD Diagnostics

```python
import blazerules

print(blazerules.simd_backend())
print(blazerules.cpu_features_summary())

cfg = blazerules.EngineConfig()
cfg.simd_backend_override = "auto"
cfg.enable_avx512 = False
```

AVX-512 is disabled for auto-selection unless explicitly enabled because some
server CPUs reduce frequency under wide vectors. Measure before enabling.

## IO Module

The full wheel and default source build include `blazerules_io`. If you maintain
a custom lean build, keep `-DBLAZERULES_IO=ON` and enable the matching decoder
flags:

```text
BLAZERULES_IO_AVRO=ON
BLAZERULES_IO_PROTOBUF=ON
```

The IO module supports:

- Kafka source/sink through librdkafka.
- Debezium CDC unwrap.
- Arrow IPC frames.
- Avro binary records.
- Protobuf binary records with descriptor sets.
- Local and exact-object `s3://` file reads.

Binary decoders produce Arrow `RecordBatch` objects and call `evaluate_batch`;
they do not need to convert through JSON.

## S3 Resources

Rules, lookup CSVs, ONNX models, and files can be loaded from exact-object
`s3://bucket/key` URIs through the AWS CLI cache path.

```python
import blazerules

blazerules.set_aws_profile("personal")
blazerules.set_aws_region("us-east-1")
blazerules.set_aws_endpoint_url("http://127.0.0.1:9000")

engine = blazerules.RuleEngine()
engine.load_rules("s3://bucket/rules/fraud.yaml")
```

Equivalent environment variables:

```bash
export BLAZERULES_AWS_PROFILE=personal
export BLAZERULES_AWS_REGION=us-east-1
export BLAZERULES_AWS_ENDPOINT_URL=http://127.0.0.1:9000
```

## Dashboard And Agent

Dashboard:

```bash
cmake --build cmake-build-release --target blazerules_dashboard -j
./cmake-build-release/blazerules_dashboard --host 127.0.0.1 --port 9470 --rules rules.yaml
```

Agent:

```bash
cmake --build cmake-build-release --target blazerules_agent -j
```

The dashboard is read-only and unauthenticated. Bind to localhost unless you add
your own network controls.

## Performance Guidance

- Use Release builds.
- Batch records; do not call the engine per record.
- Prefer Arrow when upstream data is already typed.
- Use `evaluate_ndjson(bytes_blob)` for JSON streams.
- Use `evaluate_ndjson_padded(...)` or `evaluate_ndjson_file(...)` when input is
  already simdjson-padded or memory-mapped.
- Keep streaming batches sized for latency, commonly 2K-64K rows.
- Use larger batches for throughput benchmarks.
- Use `OutputDetail.DECISIONS` unless per-rule masks are required.
- Keep partition/entity affinity for window-heavy streaming workloads.
- Avoid huge unused JSON fields when chasing JSON throughput; skipped bytes are
  still bytes the parser must scan.

## Compatibility

- Library version: `blazerules.__version__` / `blazerules.BLAZERULES_VERSION`.
- YAML compatibility: `blazerules.RULE_YAML_COMPATIBILITY`.
- Public API follows semantic versioning.
- Rule operator behavior is stable within a compatible YAML major version.
