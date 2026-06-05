# blazerules
blazerules is a C++ library, not a server or daemon. You link it into whatever application needs rule evaluation and call it like any other function. It takes a batch of records, evaluates a compiled rule set against them, and returns which rules matched, as fast as the memory bus allows.

The core idea: instead of walking one record through all your rules, blazerules sweeps each rule's condition across an entire column of values in one vectorized pass.

## What it's for
 
Any domain where a fast, deterministic rule layer sits in front of something more expensive:
 
- **Fraud pre-screening** — flag or block transactions before ML inference
- **AdTech** — bid request targeting and filtering at high throughput
- **Compliance** — classify and route events against regulatory rules
- **Insurance / FinTech** — eligibility checks, velocity scoring, risk pre-filters
## Key properties
 
- **Embeddable** — a static library you link, with no network stack, no daemon, no sidecar required
- **Columnar execution** — rules evaluate column-by-column over Arrow RecordBatches, not record-by-record
- **Declarative rules** — define rules in YAML; blazerules compiles them to an executable kernel sequence at load time
- **SIMD kernels** — AVX2 on x86-64, NEON on ARM64 (AWS Graviton); selected at runtime, no CPU-specific builds required
- **Same kernels for production and backtest** — replay historical Parquet data through the exact same compiled plan that runs in production; no train/serve skew on rule changes
- **Stateless core** — pure `batch in → result out`; deterministic and replayable by design

## Rules are YAML
 
```yaml
ruleset:
  name: "Fraud Pre-Screening"
  version: "1.0.0"
  rules:
    - id: "large_late_night_transaction"
      action: "flag"
      severity: "MEDIUM"
      conditions:
        and:
          - field: "amount"
            op: "gt"
            value: 2000.0
          - field: "account_age_days"
            op: "lt"
            value: 60
```

## Building
 
**Requirements:** C++20, CMake ≥ 3.22, and the following dependencies (see `BUILD.md` for per-platform install instructions):
 
- Apache Arrow C++ (`libarrow`)
- simdjson
- abseil-cpp (`absl::flat_hash_map`)
- Intel TBB
- yaml-cpp

## License
 
Apache 2.0
